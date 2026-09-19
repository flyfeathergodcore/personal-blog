// protoc 插件生成器：FileDescriptor → .rpc.h 桩代码文本。
// 对每个 service 生成：
//   - 服务端基类 XxxServiceBase（每个 rpc 方法一个虚函数，未实现抛 RpcException）
//   - 注册函数 RegisterXxxService(RpcServer&, XxxServiceBase&)
//   - 客户端类 XxxClient（unary 方法 / stream 方法）
// 输出 <file>.rpc.h（header-only，include 同目录 <file>.pb.h 与 rpc/codegen.h）。
#pragma once

#include <string>

#include <google/protobuf/compiler/code_generator.h>

namespace rpcgen {

// RPC 桩生成器：--rpc_out 的处理者
class RpcCodeGenerator : public google::protobuf::compiler::CodeGenerator {
public:
    // 生成 <basename>.rpc.h；参数 file - 目标 .proto 的描述；context - 输出目录
    bool Generate(const google::protobuf::FileDescriptor* file,
                  const std::string& parameter,
                  google::protobuf::compiler::GeneratorContext* context,
                  std::string* error) const override;
};

}  // namespace rpcgen
