// 生成代码依赖的注册表类型：服务端方法处理器 + 调用上下文。
// protoc 插件生成的 RegisterXxxService() 把 "pkg.Service/Method" 映射到 MethodHandler；
// 一元处理器负责 bytes 反序列化→调用用户虚函数→序列化返回；流处理器操作流对象。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "coro/task.h"
#include "rpc/rpc_stream.h"

namespace rpc {

// 服务端调用上下文：传给方法处理器的调用元信息
struct ServerCallContext {
    uint64_t call_id = 0;       // 本调用的 call_id
    std::string service_name;   // "pkg.Service"
    std::string method_name;    // "SayHello"
    std::string peer;           // 对端地址（可选填充）

    // 流方法：非空（指向连接为该调用创建的流对象）
    RpcStreamReader* reader = nullptr;
    RpcStreamWriter* writer = nullptr;
};

// 服务端方法处理器：一元 / 流两类
struct MethodHandler {
    enum class Type { Unary, Stream };

    Type type = Type::Unary;
    // 一元处理器：req 为反序列化前的请求字节，返回序列化后的响应字节
    std::function<coro::Task<std::string>(std::string req,
                                          const ServerCallContext&)> unary;
    // 流处理器：通过 ctx.reader()/ctx.writer() 读写流
    std::function<coro::Task<void>(const ServerCallContext&)> stream;
};

// ════════ 生成代码运行时支持（protoc 插件生成的 .rpc.h 依赖以下模板）════════

// 服务端类型化流读写包装：把原始字节流封装成 Req/Resp 消息流。
// 由生成代码的流 handler 构造（持 ctx.reader/writer 裸指针），传给用户基类的
// 流虚函数；Read 反序列化入 Req，Write 序列化 Resp，Finish 半关闭写出端。
template <typename Req, typename Resp>
class ServerReaderWriter {
public:
    // 构造：绑定读端/写端（来自流调用上下文）
    ServerReaderWriter(RpcStreamReader* reader, RpcStreamWriter* writer)
        : r_(reader), w_(writer) {}

    // 读一条请求：流结束（对端 DONE）返回 false；字节非法抛 RpcException(DecodeFail)
    // 参数：msg - 输出，反序列化后的请求消息
    coro::Task<bool> Read(Req* msg) {
        StreamEvent ev = co_await r_->Read(-1);
        if (ev.kind != StreamEventKind::Data) co_return false;
        if (!msg->ParseFromString(ev.payload)) {
            throw RpcException(RpcCode::DecodeFail, "bad request bytes");
        }
        co_return true;
    }

    // 写一条响应（STREAM_DATA）；流已半关闭返回 false
    // 参数：msg - 待序列化的响应消息；timeout_ms - 写超时（毫秒，<0 无限）
    coro::Task<bool> Write(const Resp& msg, int64_t timeout_ms = -1) {
        std::string bytes = msg.SerializeAsString();
        co_return co_await w_->Write(bytes, timeout_ms);
    }

    // 结束流（半关闭写出端）；幂等，只发一次
    // 参数：code - 结束状态；err - 错误消息
    coro::Task<bool> Finish(RpcCode code = RpcCode::Ok, std::string_view err = "") {
        co_return co_await w_->Finish(code, err);
    }

private:
    RpcStreamReader* r_;  // 裸指针：由框架流上下文保活（handler 期间必然存活）
    RpcStreamWriter* w_;
};

// 客户端类型化流包装：包装 RpcClientStream，序列化 Req / 反序列化 Resp。
// 由生成代码的流客户端方法构造并持有 RpcClientStream 独占所有权。
template <typename Req, typename Resp>
class ClientStream {
public:
    // 构造：接管 RpcClientStream 独占所有权（来自 OpenStream 的 unique_ptr）
    explicit ClientStream(RpcClientStream* s) : s_(s) {}
    ClientStream(const ClientStream&) = delete;             // 禁止拷贝（独占所有权）
    ClientStream& operator=(const ClientStream&) = delete;

    // 发一条请求消息（STREAM_DATA）
    // 参数：msg - 待序列化的请求；timeout_ms - 写超时（毫秒，<0 无限）
    coro::Task<bool> Write(const Req& msg, int64_t timeout_ms = -1) {
        std::string bytes = msg.SerializeAsString();
        co_return co_await s_->Write(bytes, timeout_ms);
    }

    // 读一条事件：Data 的 payload 是序列化 Resp 字节（调用方 ParseFromString）；
    // Done 携带对端结束状态；Timeout 表示读超时
    // 参数：timeout_ms - 等待上限（毫秒，<0 无限）
    coro::Task<StreamEvent> Read(int64_t timeout_ms = -1) { co_return co_await s_->Read(timeout_ms); }

    // 半关闭客户端写出端（流式请求结束后调用；仍可 Read 服务端响应）
    // 参数：timeout_ms - 写超时（毫秒，<0 无限）
    coro::Task<bool> CloseWrite(int64_t timeout_ms = -1) {
        co_return co_await s_->CloseWrite(timeout_ms);
    }

private:
    RpcClientStream* s_;
};

// 一元 handler 工厂：生成代码的 RegisterXxxService() 用它把虚函数封装成 MethodHandler。
// 内部做：bytes 反序列化 → 调用户虚函数 → 序列化返回；用户虚函数抛异常则向上传播
//（框架捕获并回错误状态）。捕获 svc/fn 两个值（拷贝），协程挂起无闭包悬垂风险。
// 参数：svc - 服务实现对象；fn - 基类一元虚函数（const Req& → Task<Resp>）
template <typename Svc, typename Req, typename Resp>
inline MethodHandler MakeUnaryHandler(Svc* svc, coro::Task<Resp> (Svc::*fn)(const Req&)) {
    MethodHandler mh;
    mh.type = MethodHandler::Type::Unary;
    mh.unary = [svc, fn](std::string req, const ServerCallContext&) -> coro::Task<std::string> {
        Req request;
        if (!request.ParseFromString(req)) {
            throw RpcException(RpcCode::DecodeFail, "bad request bytes");
        }
        Resp reply = co_await (svc->*fn)(request);
        co_return reply.SerializeAsString();
    };
    return mh;
}

// 流 handler 工厂：封装成 MethodHandler，构造 ServerReaderWriter 后调流虚函数。
// 捕获 svc/fn 两个值（拷贝），无闭包悬垂风险。
// 参数：svc - 服务实现对象；fn - 基类流虚函数（ServerReaderWriter<Req,Resp>& → Task<void>）
template <typename Svc, typename Req, typename Resp>
inline MethodHandler MakeStreamHandler(Svc* svc,
                                       coro::Task<void> (Svc::*fn)(ServerReaderWriter<Req, Resp>&)) {
    MethodHandler mh;
    mh.type = MethodHandler::Type::Stream;
    mh.stream = [svc, fn](const ServerCallContext& ctx) -> coro::Task<void> {
        ServerReaderWriter<Req, Resp> stream(ctx.reader, ctx.writer);
        co_await (svc->*fn)(stream);
    };
    return mh;
}

}  // namespace rpc
