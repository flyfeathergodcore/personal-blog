#include "server/h11_session.hpp"
#include "protocol/region_pool.hpp"
#include "server/sse_push.hpp"
#include "handler/metrics.hpp"
#include "server/ws_connection.hpp"
#include "protocol/ws_frame.hpp"
#include "coro/awaiter.h"
#include <iostream>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// 计算自给定时间点至今经过的微秒数（用于指标耗时统计）
// 参数：start - 起始时间点
static uint64_t dur_us(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

// 从连接流取对端 IPv4 字符串（getpeername）；失败返回空
// 参数：stream - 连接流（TcpStream / TlsStream 均提供 fd()）
// 返回：IPv4 点分十进制，未知返回空
template<typename Stream>
static std::string GetPeerIp(const Stream& stream)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    if (getpeername(stream.fd(),
                    reinterpret_cast<struct sockaddr*>(&peer), &len) != 0)
        return "";
    char ip[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)))
        return "";
    return ip;
}

// 构造函数：保存连接流与路由/中间件引用，按需初始化请求区域池
// 参数：stream - 连接流（TCP/TLS）；router - 路由表；middleware - 中间件管理器；region_pool - 请求区域池（可空）
template<typename Stream>
H11Session<Stream>::H11Session(Stream stream,
                               Router& router,
                               MiddlewareManager& middleware,
                               RegionPool* region_pool)
    : SessionBase(router, middleware)
    , stream_(std::move(stream))
{
    if (region_pool)
        region_.Init(region_pool);
}

// 主协程：HTTP/1.1 会话生命周期入口。循环执行 读→解析→中间件→路由→处理→发送，
// 支持 keep-alive 与 pipeline，直到连接关闭或出错
// 参数：无（基于成员状态）
template<typename Stream>
coro::Task<void> H11Session<Stream>::Start()
{
    auto self = this->shared_from_this();

    if (metrics_) metrics_->OnConnectionOpen(worker_id_);

    try {
    // 注入对端 IP（连接级不变，供访问者在线统计等按来源 IP 聚合）
    parser_.SetPeerIp(GetPeerIp(stream_));

    for (;;)
    {
        // ── 区域复位时机 ──
        // 仅在"缓冲区已耗尽"（无 pipeline 残留）且 parser 处于两条请求之间时
        // 才 Reset 区域并重新绑定 pool。若缓冲区里还有上一条请求未消费的尾部
        // （下一条请求的前缀），此时 Reset 会清掉 parser 已写入区域的数据。
        if (used_ == 0 && parser_.IsIdle()) {
            region_.Reset();
            parser_.SetPool(&region_);
        }

        // ── 缓冲区耗尽时才发起新的 read；否则用残留字节继续解析（pipeline）──
        if (used_ == 0)
        {
            auto read_start = std::chrono::steady_clock::now();
            auto r = co_await stream_.read_some(buf_.data(), buf_.size());
            if (!r.ok() || r.bytes == 0) break;
            used_ = static_cast<size_t>(r.bytes);

            // ── Raw byte phase：每个原始读块都过一遍 middleware ──
            {
                auto mw = middleware_.ProcessRaw(buf_.data(), used_);
                if (!mw.IsNone()) {
                    int code = mw.StatusCode();
                    size_t bytes = mw.HeaderWire().size() + mw.BodyWire().size();
                    co_await WriteError(std::move(mw));
                    middleware_.ExecutePostSync(parser_, code, bytes,
                        dur_us(read_start), worker_id_);
                    break;
                }
            }
        }

        auto read_start = std::chrono::steady_clock::now();

        // ── 解析：可能只消费一部分字节；未消费尾部保留给下一条请求 ──
        auto ret = parser_.Feed(buf_.data(), used_);
        size_t consumed = parser_.Consumed();
        if (consumed > used_) consumed = used_;   // 防御钳制

        // 从缓冲头部移除已消费字节，把未消费尾部（pipeline 下一条请求的前缀）
        // 搬到头部，供下一轮直接用残留字节继续解析，避免请求前缀丢失。
        size_t tail = used_ - consumed;
        if (tail > 0 && consumed > 0)
            std::memmove(buf_.data(), buf_.data() + consumed, tail);
        used_ = tail;

        if (ret == ParseResult::Incomplete) {
            // 需要更多数据：缓冲空则下一轮 read，否则继续解析残留。
            // 防御：解析器未消费任何字节却未完成（理论上不会发生），避免空转。
            if (consumed == 0) {
                co_await WriteError(400);
                middleware_.ExecutePostSync(parser_, 400, 0,
                    dur_us(read_start), worker_id_);
                break;
            }
            continue;
        }

        if (ret == ParseResult::Error) {
            int code = parser_.IsH2() ? 426 : 400;
            co_await WriteError(code);
            middleware_.ExecutePostSync(parser_, code, 0,
                dur_us(read_start), worker_id_);
            break;
        }
        if (max_body_size_ > 0 && parser_.ContentLength() > max_body_size_) {
            co_await WriteError(413);
            middleware_.ExecutePostSync(parser_, 413, 0,
                dur_us(read_start), worker_id_);
            break;
        }

        // ── PreRequest phase (sync) ──
        {
            auto pre = middleware_.ExecutePre(parser_);
            if (!pre.IsNone()) {
                int code = pre.StatusCode();
                size_t bytes = pre.HeaderWire().size() + pre.BodyWire().size();
                bool is_stream = pre.IsStream();
                co_await Send(std::move(pre));
                if (is_stream) break;
                middleware_.ExecutePostSync(parser_, code, bytes,
                    dur_us(read_start), worker_id_);

                auto conn = parser_.Header("connection");
                if (conn == "close") break;
                continue;
            }
        }

        // ── Route → Handler ──
        // Match 捕获路径参数（:id / *）并注入 parser_（即 Context），
        // handler 通过 ctx.Param("id") 获取，无需自解析 Path。
        auto resp = Response::None();
        std::vector<std::pair<std::string_view, std::string_view>> params;
        auto* handler = router_.Match(parser_.Method(), parser_.Path(), &params);
        parser_.SetParams(params);

        // ── WebSocket 代理直连 ──
        // 请求为 WS upgrade 且 handler 声明直接处理（ReverseProxy 等）时，
        // 直接进 HandleWebSocket，由 handler 自行完成与上游的握手 + 中继。
        // 不 reset region：HandleWebSocket 需读 ctx 的 Method/Path/Header 组装上游请求。
        if (handler && handler->IsWebSocketUpgradeHandler())
        {
            std::string up   = std::string(parser_.Header("upgrade"));
            std::string conn = std::string(parser_.Header("connection"));
            std::string key  = std::string(parser_.Header("sec-websocket-key"));
            for (auto& c : up)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : conn)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool is_ws_upgrade = !key.empty()
                                 && up.find("websocket") != std::string::npos
                                 && conn.find("upgrade") != std::string::npos;
            if (is_ws_upgrade)
            {
                WsConnection<Stream> ws_conn(stream_, ws_idle_timeout_);
                co_await handler->HandleWebSocket(parser_, ws_conn);
                break;
            }
        }

        if (handler && handler->IsStream()) {
            // ── 流式路径：写 SSE 响应头 → 让 Handler 驱动输出 ──
            // 注意：不能走 Send()，Send() 的 stream 分支是 metrics 推送循环；
            // 这里只写响应头，body 由 handler 通过 sink 驱动。
            auto sse_resp = Response::SSEStream(region_, 0);
            (void)co_await stream_.write_all(sse_resp.HeaderWire());
            H1StreamSink<Stream> sink(stream_);
            co_await handler->HandleStream(parser_, sink);
            break;  // SSE 结束后不再 keep-alive
        } else if (handler && handler->IsAsync()) {
            resp = co_await handler->HandleAsync(parser_);
        } else if (handler) {
            resp = handler->Handle(parser_);
        } else {
            resp = Response::Error(404, region_);
        }

        // ── WebSocket upgrade? (handler decided, session executes) ──
        if (resp.IsWebSocket()) {
            co_await WriteError(std::move(resp));  // write 101
            region_.Reset();
            WsConnection<Stream> ws_conn(stream_, ws_idle_timeout_);
            if (handler)
                co_await handler->HandleWebSocket(parser_, ws_conn);
            break;
        }

        int code = resp.StatusCode();
        size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size()
                     + (resp.IsFile() ? resp.FileSize() : 0);
        bool is_stream = resp.IsStream();
        co_await Send(std::move(resp));

        if (is_stream) break;

        // ── PostResponse phase ──
        middleware_.ExecutePostSync(parser_, code, bytes,
            dur_us(read_start), worker_id_);

        auto conn = parser_.Header("connection");
        if (conn == "close") break;
    }
    } catch (std::exception& e) {
        std::cerr << "[session] " << e.what() << std::endl;
    }

    if (metrics_) metrics_->OnConnectionClose(worker_id_);
    co_return;
}

// 向对端写入固定状态码的错误响应（400/413/426/500），每次连接至多一次
// 参数：code - HTTP 状态码（其他取值按 500 处理）
template<typename Stream>
coro::Task<void> H11Session<Stream>::WriteError(int code)
{
    // 单一数据源：Content-Length 一律按 body.size() 计算，杜绝硬编码 CL 与
    // 实际 body 字节数不一致（curl 会以 "transfer closed with N bytes remaining"
    // 中止）。保持 switch 结构，仅把各分支拆成 status_line + body。
    std::string status_line;
    std::string body;
    switch (code) {
        case 400:
            status_line = "HTTP/1.1 400 Bad Request\r\n";
            body        = "Bad Request\r\n";
            break;
        case 413:
            status_line = "HTTP/1.1 413 Payload Too Large\r\n";
            body        = "Payload Too Large\r\n";
            break;
        case 426:
            status_line = "HTTP/1.1 426 Upgrade Required\r\n";
            body        = "HTTP/2 is not supported yet. Use HTTP/1.1.\r\n";
            break;
        default:
            status_line = "HTTP/1.1 500 Internal Server Error\r\n";
            body        = "Internal Server Error\r\n";
            break;
    }
    std::string wire = status_line
        + "Content-Type: text/plain\r\n"
        + "Content-Length: " + std::to_string(body.size()) + "\r\n"
        + "Connection: close\r\n"
        + "\r\n"
        + body;
    (void)co_await stream_.write_all(wire);
    co_return;
}

// 向对端写入中间件/自定义构造的完整响应（raw middleware 或 101 upgrade）
// 参数：response - 预构造的响应对象
template<typename Stream>
coro::Task<void> H11Session<Stream>::WriteError(Response response)
{
    auto hw = response.HeaderWire();
    auto bw = response.BodyWire();
    // 头 + 体合并为单次 writev，省一次 syscall（body 为 region/FileCache 引用，零拷贝）。
    if (!bw.empty())
        (void)co_await stream_.writev_all({hw, bw});
    else
        (void)co_await stream_.write_all(hw);
    co_return;
}

// 发送完整响应：文件走 sendfile/read 零拷贝、流走 SSE 推送循环、普通响应头+体合并写
// 参数：response - 待发送的响应对象
template<typename Stream>
coro::Task<void> H11Session<Stream>::Send(Response response)
{
    if (response.IsFile())
    {
        if (!(co_await stream_.write_all(response.HeaderWire()))) co_return;

        auto fd = response.Fd();
        auto range_off = response.FileRangeOffset();
        auto remaining = (response.FileRangeLen() > 0)
                       ? response.FileRangeLen()
                       : response.FileSize();

        if constexpr (std::is_same_v<Stream, net::TcpStream>)
        {
#ifdef __linux__
            off_t offset = static_cast<off_t>(range_off);
            while (remaining > 0) {
                ssize_t n = ::sendfile(stream_.fd(), fd, &offset, remaining);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // 非阻塞 socket 写缓冲满：等待可写后重试
                        (void)co_await coro::await_event(
                            stream_.fd(), coro::IoPoller::WRITE, -1);
                        continue;
                    }
                    break;
                }
                if (n == 0) break;
                remaining -= static_cast<size_t>(n);
            }
#else
            // macOS 无 Linux 版 sendfile（签名与语义不同），退化为 read+write
            // 循环（同下方通用流路径），牺牲零拷贝换取可移植性。
            ::lseek(fd, static_cast<off_t>(range_off), SEEK_SET);
            std::array<char, 65536> readbuf;
            while (remaining > 0) {
                auto to_read = std::min(remaining, readbuf.size());
                ssize_t n = ::read(fd, readbuf.data(), to_read);
                if (n <= 0) break;
                if (!(co_await stream_.write_all(
                        {readbuf.data(), static_cast<size_t>(n)})))
                    break;
                remaining -= static_cast<size_t>(n);
            }
#endif
        }
        else
        {
            ::lseek(fd, static_cast<off_t>(range_off), SEEK_SET);
            std::array<char, 65536> readbuf;
            while (remaining > 0) {
                auto to_read = std::min(remaining, readbuf.size());
                ssize_t n = ::read(fd, readbuf.data(), to_read);
                if (n <= 0) break;
                if (!(co_await stream_.write_all(
                        {readbuf.data(), static_cast<size_t>(n)})))
                    break;
                remaining -= static_cast<size_t>(n);
            }
        }
    }
    else if (response.IsStream())
    {
        if (!(co_await stream_.write_all(response.HeaderWire()))) co_return;

        // Send initial SSE payload
        {
            auto init = SseInitialPayload(metrics_);
            if (!init.empty())
                if (!(co_await stream_.write_all(init))) co_return;
        }

        int push_ms = response.PushIntervalMs();
        SsePushState sse;

        for (;;)
        {
            co_await coro::sleep_for(push_ms);

            if (!metrics_) break;

            auto payload = sse.BuildPayload(metrics_);
            if (payload.empty()) continue;  // nothing new, wait for next tick

            if (!(co_await stream_.write_all(payload))) break;
        }
    }
    else
    {
        auto body = response.BodyWire();
        if (!body.empty())
        {
            // 头 + 零拷贝 body 合并为单次 writev，省一次 syscall。
            if (!(co_await stream_.writev_all({response.HeaderWire(), body}))) co_return;
        }
        else
        {
            if (!(co_await stream_.write_all(response.HeaderWire()))) co_return;
        }
    }
    co_return;
}

template class H11Session<net::TcpStream>;
template class H11Session<net::TlsStream>;
