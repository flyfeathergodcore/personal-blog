#pragma once
#include "handler/request_handler.hpp"
#include "server/session_base.hpp"
#include "protocol/http1.1/parser.hpp"
#include "net/tcp_stream.h"
#include "net/tls_stream.h"
#include "coro/task.h"
#include <array>

class RegionPool;

template<typename Stream>
class H11Session : public SessionBase {
public:
    H11Session(Stream stream,
               Router& router,
               MiddlewareManager& middleware,
               RegionPool* region_pool = nullptr);

    coro::Task<void> Start() override;

    /// Reuse session shell: replace stream, reuse existing parser.
    void Reset(Stream stream) {
        stream_ = std::move(stream);
        used_ = 0;
        // 复用前复位 parser：上一连接若在请求中途断开（state_ 停在 HEADERS/BODY），
        // Feed 只对 DONE/ERROR 态自复位，必须显式 Reset 清掉旧状态机，否则
        // 新连接的首个请求会被误解析；同时保证 Start() 首轮 IsIdle() 为真，
        // 区域池能正常 Reset。
        parser_.Reset();
    }

private:
    Stream stream_;
    H1Parser parser_;

    // 持久读缓冲（原为 Start() 内栈上局部数组）。HTTP/1.1 pipeline 场景下，
    // 一个 TCP 段可能含多条请求：解析器消费掉第一条后，未消费的尾部必须保留，
    // 在下一轮直接用残留字节继续解析（不发起新 read）。used_ 是缓冲有效字节数。
    static constexpr size_t kReadBufSize = 4096;
    std::array<char, kReadBufSize> buf_;
    size_t used_ = 0;

    // Error write — known code (400/413/426/500), once per connection, coroutine frame cost OK
    coro::Task<void> WriteError(int code);
    // Middleware / custom response write (raw middleware, 101 upgrade)
    coro::Task<void> WriteError(Response resp);
    // Full send — file (sendfile), stream (SSE), or regular header+body
    coro::Task<void> Send(Response response);
};

// ═══════════════════════════════════════════════════════════════════
// H1StreamSink — H1 的 StreamSink 实现
// 类型擦除：Stream 可以是 net::TcpStream 或 net::TlsStream
// ═══════════════════════════════════════════════════════════════════

template<typename Stream>
class H1StreamSink : public StreamSink {
public:
    explicit H1StreamSink(Stream& stream)
        : stream_(stream) {}

    coro::Task<bool> Write(std::string_view data) override {
        if (disconnected_) co_return false;
        bool ok = co_await stream_.write_all(data);
        if (!ok) disconnected_ = true;
        co_return ok;
    }

    void End() override { disconnected_ = true; }

    bool IsDisconnected() const override { return disconnected_; }

private:
    Stream& stream_;
    bool disconnected_ = false;
};
