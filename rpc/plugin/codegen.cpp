// protoc 插件生成器实现：遍历 FileDescriptor 生成 .rpc.h 桩代码文本。
// 生成内容依赖框架运行时模板（rpc/codegen.h 的 ServerReaderWriter/ClientStream/
// MakeUnaryHandler/MakeStreamHandler），生成的类放全局命名空间、类型用全限定名
//（::pkg::Message），避免包名解析歧义。
#include "codegen.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream.h>

namespace rpcgen {

namespace {

// protobuf 35 的 descriptor 返回 absl::string_view：统一转 std::string 便于拼接。
// 用 data()/size() 构造：避免依赖 absl::string_view 到 std::string 的隐式转换
std::string Str(absl::string_view sv) { return std::string(sv.data(), sv.size()); }

// Descriptor → 全限定 C++ 类型名：full_name "greeter.HelloRequest" → "::greeter::HelloRequest"
// 注意：replace 单字符会把 '.' 换成单个 ':'，必须用子串替换成 "::"
std::string CppTypeName(const google::protobuf::Descriptor* d) {
    const std::string full = Str(d->full_name());
    std::string out;
    out.reserve(full.size() + 4);
    out += "::";
    for (char c : full) {
        if (c == '.') {
            out += "::";
        } else {
            out += c;
        }
    }
    return out;
}

// 服务全名（注册键）：package + "." + service name → "greeter.Greeter"
std::string ServiceFullName(const google::protobuf::ServiceDescriptor* s) {
    const std::string pkg = Str(s->file()->package());
    return pkg.empty() ? Str(s->name()) : pkg + "." + Str(s->name());
}

// 服务端基类名：XxxServiceBase；service 名已以 "Service" 结尾则不重复后缀
// （Greeter → GreeterServiceBase；RegistryService → RegistryServiceBase）
std::string BaseClassName(const std::string& svc_name) {
    if (svc_name.size() >= 7 && svc_name.compare(svc_name.size() - 7, 7, "Service") == 0)
        return svc_name + "Base";
    return svc_name + "ServiceBase";
}

// 去掉 service 名末尾 "Service" 后缀（用于组装注册函数名）
// （Greeter → Greeter；RegistryService → Registry）
std::string StripServiceSuffix(const std::string& svc_name) {
    if (svc_name.size() >= 7 && svc_name.compare(svc_name.size() - 7, 7, "Service") == 0)
        return svc_name.substr(0, svc_name.size() - 7);
    return svc_name;
}

// package → 嵌套 namespace 开块：C++ 命名空间不含 '.'，"rpc.registry" 拆成两层
// 输出形如 "namespace rpc {\nnamespace registry {\n"
std::string NamespaceOpen(const std::string& pkg) {
    std::ostringstream os;
    std::string cur;
    for (char c : pkg) {
        if (c == '.') {
            os << "namespace " << cur << " {\n";
            cur.clear();
        } else {
            cur += c;
        }
    }
    os << "namespace " << cur << " {\n";
    return os.str();
}

// 对应的收块（逆序）："rpc.registry" → "}  // namespace registry\n}  // namespace rpc\n"
std::string NamespaceClose(const std::string& pkg) {
    std::ostringstream os;
    std::string cur;
    for (auto it = pkg.rbegin(); it != pkg.rend(); ++it) {
        if (*it == '.') {
            os << "}  // namespace " << cur << "\n";
            cur.clear();
        } else {
            cur = std::string(1, *it) + cur;  // 逆序拼回原段
        }
    }
    os << "}  // namespace " << cur << "\n";
    return os.str();
}

// 生成单个 service 的 .rpc.h 片段（服务端基类 + 注册函数 + 客户端类）
std::string GenerateService(const google::protobuf::ServiceDescriptor* svc) {
    const std::string name = Str(svc->name());
    const std::string base = BaseClassName(name);               // GreeterServiceBase / RegistryServiceBase
    const std::string client = StripServiceSuffix(name) + "Client";  // GreeterClient / RegistryClient
    const std::string full = ServiceFullName(svc);              // greeter.Greeter
    const std::string reg_fn = "Register" + StripServiceSuffix(name) + "Service";

    std::ostringstream os;
    os << "\n// ════ 服务端 " << base << " ════\n";
    os << "class " << base << " {\n";
    os << "public:\n";
    os << "    virtual ~" << base << "() = default;\n";

    for (int i = 0; i < svc->method_count(); ++i) {
        const google::protobuf::MethodDescriptor* m = svc->method(i);
        const std::string req = CppTypeName(m->input_type());
        const std::string resp = CppTypeName(m->output_type());
        const bool is_stream = m->client_streaming() || m->server_streaming();
        const std::string path = full + "/" + Str(m->name());

        if (is_stream) {
            // 流方法：统一接收类型化流对象（bidi / client / server streaming 通用）
            os << "    // " << (m->client_streaming() && m->server_streaming() ? "双向流" :
                                m->client_streaming() ? "客户端流" : "服务端流")
               << "：" << m->name() << "（未实现抛 RpcException）\n";
            os << "    virtual coro::Task<void> " << m->name()
               << "(rpc::ServerReaderWriter<" << req << ", " << resp
               << ">& stream) {\n";
            os << "        throw rpc::RpcException(rpc::RpcCode::NotFound, \"" << path
               << " not implemented\");\n";
            os << "    }\n";
        } else {
            // 一元方法
            os << "    // 一元：" << m->name() << "（未实现抛 RpcException）\n";
            os << "    virtual coro::Task<" << resp << "> " << m->name()
               << "(const " << req << "& request) {\n";
            os << "        throw rpc::RpcException(rpc::RpcCode::NotFound, \"" << path
               << " not implemented\");\n";
            os << "    }\n";
        }
    }
    os << "};\n\n";

    // 注册函数：把虚函数封装成 MethodHandler 注册进 server
    os << "inline void " << reg_fn << "(rpc::RpcServer& server, "
       << base << "& svc) {\n";
    for (int i = 0; i < svc->method_count(); ++i) {
        const google::protobuf::MethodDescriptor* m = svc->method(i);
        const std::string req = CppTypeName(m->input_type());
        const std::string resp = CppTypeName(m->output_type());
        const bool is_stream = m->client_streaming() || m->server_streaming();
        os << "    server.Register(\"" << full << "\", \"" << m->name() << "\", rpc::"
           << (is_stream ? "MakeStreamHandler" : "MakeUnaryHandler")
           << "(&svc, &" << base << "::" << m->name() << "));\n";
    }
    os << "}\n\n";

    // 客户端类：方法声明
    os << "// ════ 客户端 " << client << " ════\n";
    os << "class " << client << " {\n";
    os << "public:\n";
    os << "    // 构造：绑定客户端通道（连接由调用方管理）\n";
    os << "    explicit " << client << "(rpc::RpcChannel& ch) : ch_(&ch) {}\n";
    for (int i = 0; i < svc->method_count(); ++i) {
        const google::protobuf::MethodDescriptor* m = svc->method(i);
        const std::string req = CppTypeName(m->input_type());
        const std::string resp = CppTypeName(m->output_type());
        const bool is_stream = m->client_streaming() || m->server_streaming();
        if (is_stream) {
            os << "    // " << m->name() << "：开启双向流\n";
            os << "    // 参数：timeout_ms - 开流超时（毫秒，默认 5000）\n";
            os << "    coro::Task<std::unique_ptr<rpc::ClientStream<" << req << ", " << resp
               << ">>> " << m->name() << "(int64_t timeout_ms = 5000);\n";
        } else {
            os << "    // " << m->name() << "：一元调用\n";
            os << "    // 参数：request - 请求消息；timeout_ms - 调用超时（毫秒，默认 5000）\n";
            os << "    coro::Task<" << resp << "> " << m->name()
               << "(const " << req << "& request, int64_t timeout_ms = 5000);\n";
        }
    }
    os << "private:\n";
    os << "    rpc::RpcChannel* ch_;  // 借用：连接生命周期由调用方保证\n";
    os << "};\n\n";

    // 客户端方法实现
    for (int i = 0; i < svc->method_count(); ++i) {
        const google::protobuf::MethodDescriptor* m = svc->method(i);
        const std::string req = CppTypeName(m->input_type());
        const std::string resp = CppTypeName(m->output_type());
        const bool is_stream = m->client_streaming() || m->server_streaming();
        if (is_stream) {
            os << "inline coro::Task<std::unique_ptr<rpc::ClientStream<" << req << ", " << resp
               << ">>> " << client << "::" << m->name() << "(int64_t timeout_ms) {\n";
            os << "    auto s = co_await ch_->OpenStream(\"" << full << "\", \"" << m->name()
               << "\", timeout_ms);\n";
            os << "    co_return std::make_unique<rpc::ClientStream<" << req << ", " << resp
               << ">>(s.release());\n";
            os << "}\n\n";
        } else {
            os << "inline coro::Task<" << resp << "> " << client << "::" << m->name()
               << "(const " << req << "& request, int64_t timeout_ms) {\n";
            os << "    std::string resp = co_await ch_->UnaryCall(\"" << full << "\", \""
               << m->name() << "\", request.SerializeAsString(), timeout_ms);\n";
            os << "    " << resp << " reply;\n";
            os << "    if (!reply.ParseFromString(resp)) {\n";
            os << "        throw rpc::RpcException(rpc::RpcCode::DecodeFail, \"bad response bytes\");\n";
            os << "    }\n";
            os << "    co_return reply;\n";
            os << "}\n\n";
        }
    }
    return os.str();
}

}  // namespace

// 生成 <basename>.rpc.h：遍历所有 service 输出桩代码
bool RpcCodeGenerator::Generate(const google::protobuf::FileDescriptor* file,
                                const std::string& parameter,
                                google::protobuf::compiler::GeneratorContext* context,
                                std::string* error) const {
    (void)parameter;
    if (file->service_count() == 0) return true;  // 无 service：不生成

    // 输出文件名：取源文件 basename，去 .proto 后缀 → <name>.rpc.h
    std::string name = Str(file->name());
    std::string base = name;
    const std::string::size_type slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() <= 6 || base.compare(base.size() - 6, 6, ".proto") != 0) {
        if (error) *error = "rpc plugin: expected .proto file, got: " + name;
        return false;
    }
    std::string out_name = base.substr(0, base.size() - 6) + ".rpc.h";  // "greeter" + ".rpc.h"
    const std::string pb_include = base.substr(0, base.size() - 6) + ".pb.h";

    std::ostringstream os;
    os << "// 自动生成：" << out_name << " — 请勿手改（protoc --rpc_out 生成，依赖 rpc 框架）\n";
    os << "#pragma once\n\n";
    os << "#include <memory>\n";
    os << "#include <string>\n";
    os << "#include \"coro/task.h\"\n";
    os << "#include \"rpc/codegen.h\"\n";
    os << "#include \"rpc/rpc_channel.h\"\n";
    os << "#include \"rpc/rpc_server.h\"\n";
    os << "#include \"" << pb_include << "\"\n";

    // 生成的服务端/客户端类放 package 命名空间（与消息类型一致，避免全局污染）；
    // package 含 '.'（如 rpc.registry）时按段拆成嵌套 namespace；无 package 时生成在全局
    const std::string pkg = Str(file->package());
    if (!pkg.empty()) os << "\n" << NamespaceOpen(pkg);
    for (int i = 0; i < file->service_count(); ++i) {
        os << GenerateService(file->service(i));
    }
    if (!pkg.empty()) os << "\n" << NamespaceClose(pkg);

    // Open 返回裸指针且调用者负责所有权（protobuf 35 的 GeneratorContext 约定）
    google::protobuf::io::ZeroCopyOutputStream* raw = context->Open(out_name);
    if (raw == nullptr) {
        if (error) *error = "rpc plugin: cannot open output file: " + out_name;
        return false;
    }
    std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> out(raw);
    // ZeroCopyOutputStream 无 Write 便捷方法：用 Next/BackUp 分块写入
    const std::string content = os.str();
    const char* data = content.data();
    int remaining = static_cast<int>(content.size());
    while (remaining > 0) {
        void* ptr = nullptr;
        int n = 0;
        if (!out->Next(&ptr, &n)) {
            if (error) *error = "rpc plugin: write failed: " + out_name;
            return false;
        }
        const int chunk = remaining < n ? remaining : n;
        std::memcpy(ptr, data, static_cast<size_t>(chunk));
        data += chunk;
        remaining -= chunk;
        if (chunk < n) out->BackUp(n - chunk);  // 回退未用尽的缓冲区
    }
    return true;
}

}  // namespace rpcgen
