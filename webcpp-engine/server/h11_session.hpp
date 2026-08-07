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
    // 构造函数：保存连接流与路由/中间件引用，按需初始化请求区域池
    // 参数：stream - 连接流（TCP/TLS）；router - 路由表；middleware - 中间件管理器；region_pool - 请求区域池（可空）
    H11Session(Stream stream,
               Router& router,
               MiddlewareManager& middleware,
               RegionPool* region_pool = nullptr);

    // 主协程：处理整个 HTTP/1.1 连接生命周期（读→解析→路由→处理→发送）
    // 参数：无
    coro::Task<void> Start() override;

    // 复用 Session 外壳：替换底层流、复用已有 parser（供连接池取回后重新挂载）
    // 参数：stream - 新的连接流
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

    // 写固定状态码错误响应（400/413/426/500），每次连接至多一次，协程帧开销可接受
    // 参数：code - HTTP 状态码
    coro::Task<void> WriteError(int code);
    // 写中间件/自定义构造的完整响应（raw middleware、101 upgrade）
    // 参数：resp - 预构造的响应对象
    coro::Task<void> WriteError(Response resp);
    // 发送完整响应：文件（sendfile）、流（SSE）或普通头+体
    // 参数：response - 待发送的响应对象
    coro::Task<void> Send(Response response);
};

// ═══════════════════════════════════════════════════════════════════
// H1StreamSink — H1 的 StreamSink 实现
// 类型擦除：Stream 可以是 net::TcpStream 或 net::TlsStream
// ═══════════════════════════════════════════════════════════════════

template<typename Stream>
class H1StreamSink : public StreamSink {
public:
    // 构造函数：绑定底层流，供以类型擦除方式写入 SSE/流式响应
    // 参数：stream - 底层流引用
    explicit H1StreamSink(Stream& stream)
        : stream_(stream) {}

    // 写入一段数据到底层流；已断开或写入失败时返回 false 并置断开标记
    // 参数：data - 待写入数据；返回：写入成功与否
    coro::Task<bool> Write(std::string_view data) override {
        if (disconnected_) co_return false;
        bool ok = co_await stream_.write_all(data);
        if (!ok) disconnected_ = true;
        co_return ok;
    }

    // 结束流：标记断开，后续 Write 直接返回失败
    // 参数：无
    void End() override { disconnected_ = true; }

    // 查询流是否已断开
    // 参数：无；返回：是否断开
    bool IsDisconnected() const override { return disconnected_; }

private:
    Stream& stream_;
    bool disconnected_ = false;
};
