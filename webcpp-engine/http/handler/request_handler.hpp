#pragma once
#include <functional>
#include <string>
#include "coro/task.h"
#include "http/protocol/response.hpp"
#include "http/protocol/context.hpp"
#include "http/cache/file_cache.hpp"
#include "http/config/config.hpp"
#include "http/server/ws_connection.hpp"

// ── 前向声明 ──
class StreamSink;

// ═══════════════════════════════════════════════════════════════════
// RequestHandler — abstract interface for HTTP request handlers
//
// Two execution paths:
//   Handle()       — sync, for CPU-bound handlers (static files, etc.)
//   HandleAsync()  — async coroutine, for I/O-bound handlers (proxy, etc.)
//
// Override IsAsync() to return true when using HandleAsync, so the
// session dispatcher picks the coroutine path.
// ═══════════════════════════════════════════════════════════════════

class RequestHandler {
public:
    // 虚析构，确保派生类正确释放
    virtual ~RequestHandler() = default;

    /// Fast sync path — override for CPU-bound handlers.
    /// Pure virtual: every handler must implement at least this.
    virtual Response Handle(const Context& ctx) = 0;

    /// Async coroutine path — override for I/O-bound handlers (proxy, etc.).
    /// Default implementation wraps Handle() — callable from any handler
    /// regardless of IsAsync(), but the coroutine frame overhead only pays
    /// off when overridden.
    virtual coro::Task<Response> HandleAsync(const Context& ctx) {
        co_return Handle(ctx);
    }

    /// Returns true if this handler should use the async path.
    /// Session checks this to decide whether to co_await HandleAsync().
    virtual bool IsAsync() const { return false; }

    // ── 流式路径 ──
    /// Returns true if this handler uses streaming output.
    virtual bool IsStream() const { return false; }

    /// 本 handler 是否直接处理 WebSocket upgrade（如反向代理透传）。
    /// 为 true 且入站请求是 WS upgrade 时，会话直接走 HandleWebSocket()，
    /// 不经过 Handle()/HandleAsync()（后者无法产生 101 升级响应）。
    virtual bool IsWebSocketUpgradeHandler() const { return false; }

    /// Streaming entry point.
    /// Handler writes chunks via sink; session does not call Send() afterwards.
    /// Default fallback: collect full response from HandleAsync() then flush.
    virtual coro::Task<void> HandleStream(const Context& ctx,
                                          StreamSink& sink);

    /// WebSocket handler — session calls this after 101 handshake.
    /// Default empty: handler does not support WebSocket.
    /// Override to read/write frames via the connection object.
    /// @param ctx  The original HTTP upgrade request context
    /// @param conn WebSocket connection for frame read/write
    virtual coro::Task<void> HandleWebSocket(const Context& /*ctx*/,
                                             WsConnectionBase& /*conn*/) {
        co_return;
    }
};

// ═══════════════════════════════════════════════════════════════════
// StaticFileHandler — serves static files from a FileCache
// ═══════════════════════════════════════════════════════════════════

class StaticFileHandler : public RequestHandler {
public:
    // 构造静态文件处理器
    // 参数：cache - 文件缓存指针（不拥有，需存活于 handler 生命周期内）
    explicit StaticFileHandler(const FileCache* cache);
    // 同步处理静态文件请求，从 FileCache 取文件内容并构造响应
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override;

private:
    const FileCache* cache_;
    // 规范化请求路径，去除 /../ 等危险片段，防止路径穿越
    // 参数：raw - 原始请求路径；返回规范化后的路径
    std::string NormalizePath(std::string_view raw) const;
};

// ═══════════════════════════════════════════════════════════════════
// NullHandler — returns 404 for every request (router fallback)
// ═══════════════════════════════════════════════════════════════════

class NullHandler : public RequestHandler {
public:
    // 兜底处理任何请求，一律返回 404
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override {
        return Response::Error(404, *ctx.Pool());
    }
};

// ═══════════════════════════════════════════════════════════════════
// RedirectHandler — returns 301/302/307/308 + Location header
// ═══════════════════════════════════════════════════════════════════

class RedirectHandler : public RequestHandler {
public:
    // 构造重定向处理器
    // 参数：target - 重定向目标 URL；code - 状态码（默认 302）
    explicit RedirectHandler(std::string target, int code = 302)
        : target_(std::move(target)), code_(code) {}
    // 返回带 Location 头的重定向响应
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override;
private:
    std::string target_;
    int code_;
};

// ═══════════════════════════════════════════════════════════════════
// FunctionHandler — 包装普通可调用对象为 RequestHandler
//
// 供 Router 的 lambda/函数式注册接口使用（router.Get("/x", [](const Context&)
// -> Response {...})）。仅同步路径；异步/流式/WebSocket 仍需类 handler。
// ═══════════════════════════════════════════════════════════════════

class FunctionHandler : public RequestHandler {
public:
    // 构造函数式处理器
    // 参数：fn - 签名为 Response(const Context&) 的可调用对象
    explicit FunctionHandler(std::function<Response(const Context&)> fn)
        : fn_(std::move(fn)) {}
    // 同步调用被包装的函数并返回其结果
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override { return fn_(ctx); }
private:
    std::function<Response(const Context&)> fn_;
};

// ═══════════════════════════════════════════════════════════════════
// StreamSink — 流式输出通道
//
// Handler 通过此接口分块写回客户端。
// H1/H2 Session 各自提供具体实现，擦除底层流类型差异。
// ═══════════════════════════════════════════════════════════════════

class StreamSink {
public:
    // 虚析构，确保派生类正确释放
    virtual ~StreamSink() = default;

    /// 写原始 bytes 到响应流
    /// @return true 成功，false 连接断开或出错
    virtual coro::Task<bool> Write(std::string_view data) = 0;

    /// 写 SSE 格式数据：自动包装为 "data: <content>\n\n"
    virtual coro::Task<bool> PushSSE(std::string_view data) {
        std::string frame = "data: ";
        frame.append(data.data(), data.size());
        frame.append("\n\n");
        co_return co_await Write(frame);
    }

    /// 关闭流
    virtual coro::Task<void> End() = 0;

    /// 客户端是否已断开
    virtual bool IsDisconnected() const = 0;
};

// ── RequestHandler::HandleStream 默认实现（需要 StreamSink 完整定义）──

inline coro::Task<void> RequestHandler::HandleStream(
    const Context& ctx, StreamSink& sink) {
    auto resp = co_await HandleAsync(ctx);
    co_await sink.Write(resp.BodyWire());
    co_await sink.End();
}
