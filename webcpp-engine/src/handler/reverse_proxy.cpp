#include "handler/reverse_proxy.hpp"
#include "server/upstream_conn_pool.hpp"
#include "protocol/response.hpp"
#include "protocol/session_region.hpp"
#include "server/ws_relay.hpp"
#include "protocol/context.hpp"
#include "net/resolver.h"
#include "net/buffered_reader.h"
#include "net/when_all.h"
#include "protocol/ws_frame.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

// ── Helpers ──

// 从缓冲区当前位置读取一行（CRLF 结尾，不含换行符），成功返回 true 并推进 pos
// 参数：buf - 缓冲区内容；pos - 读取起始位置（成功后被推进到下一行）；line - 输出行内容
static bool ReadLine(const std::string& buf, size_t& pos, std::string& line)
{
    auto cr = buf.find('\r', pos);
    if (cr == std::string::npos || cr + 1 >= buf.size() || buf[cr + 1] != '\n')
        return false;
    line = buf.substr(pos, cr - pos);
    pos = cr + 2;
    return true;
}

/// 读取并解析 chunked 编码的 body，还原为纯字节写入 out。
/// 返回 false 表示上游格式错误 / 连接中断。
/// 参数：reader - 上游响应读取器；out - 还原后的 body 字节
static coro::Task<bool> ReadChunkedBody(net::BufferedReader& reader,
                                        std::string& out)
{
    for (;;) {
        std::string line;
        auto r = co_await reader.read_until("\r\n", line);
        if (!r.ok()) co_return false;

        // 解析 16 进制块长（分号后为扩展，忽略）
        size_t sz = 0;
        size_t i = 0;
        for (; i < line.size() && line[i] != ';'; ++i) {
            char c = line[i];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (sz > (SIZE_MAX - static_cast<size_t>(d)) / 16)
                co_return false;   // 溢出防护
            sz = sz * 16 + static_cast<size_t>(d);
        }

        if (sz == 0) {
            // 终止块：读 trailer 直到空行
            for (;;) {
                std::string tl;
                auto rt = co_await reader.read_until("\r\n", tl);
                if (!rt.ok()) co_return false;
                if (tl.empty()) co_return true;
            }
        }

        std::string chunk;
        auto rc = co_await reader.read_exact(sz, chunk);
        if (!rc.ok() || chunk.size() < sz) co_return false;
        out.append(chunk);

        // 块数据后的 CRLF
        std::string crlf;
        auto rl = co_await reader.read_until("\r\n", crlf);
        if (!rl.ok()) co_return false;
    }
}

// ── WS 帧读取源 ──
// 先消费 101 握手后已缓冲的上游字节，再回落到底层 socket。避免 read_until
// 读 101 头时把紧随其后的首帧字节吞进 BufferedReader 缓冲，导致后续
// ReadFrame 直接从 socket 读时丢失这些字节。
template<typename Stream>
struct FrameSource {
    std::string pending;
    size_t pos = 0;
    Stream& inner;

    // 读取字节：先消费 101 握手后已缓冲的上游字节，耗尽后才回落到底层 socket
    // 参数：buf - 输出缓冲区；n - 请求读取字节数；timeout_ms - 超时（-1 不超时）
    coro::Task<net::IoResult> read_some(void* buf, size_t n,
                                        int64_t timeout_ms = -1) {
        if (pos < pending.size()) {
            size_t take = std::min(n, pending.size() - pos);
            std::memcpy(buf, pending.data() + pos, take);
            pos += take;
            co_return net::IoResult{take, net::IoError::None};
        }
        co_return co_await inner.read_some(buf, n, timeout_ms);
    }

    // 原样透传写入到底层流
    // 参数：data - 待写入数据；timeout_ms - 超时（-1 不超时）
    coro::Task<bool> write_all(std::string_view data, int64_t timeout_ms = -1) {
        co_return co_await inner.write_all(data, timeout_ms);
    }
};

// ── 响应头解析 ──
// 提取 framing 信息（Content-Length / Transfer-Encoding / Connection），
// 并收集要透传的头（跳过 hop-by-hop 与由调用方控制 framing 的头）。
struct ResponseFraming {
    size_t content_length = 0;
    bool has_content_length = false;
    bool chunked = false;
    bool upstream_close = false;
    bool upstream_keepalive = false;
    std::vector<std::pair<std::string, std::string>> headers;
};

// 解析上游响应头：提取 framing 信息（Content-Length/Transfer-Encoding/Connection），
// 收集待透传的头（跳过 hop-by-hop 与由调用方控制的 framing 头）
// 参数：hdr_block_in - 原始头块字符串；f - 解析结果输出
static void ParseResponseHeaders(const std::string& hdr_block_in, ResponseFraming& f)
{
    // read_until("\r\n\r\n") 消费了分隔符，末行头无尾随 \r\n；补齐后每行都能被 ReadLine 解析
    std::string hdr_block = hdr_block_in;
    hdr_block += "\r\n";
    size_t hp = 0;
    while (hp < hdr_block.size()) {
        std::string hdr_line;
        if (!ReadLine(hdr_block, hp, hdr_line)) break;
        auto colon = hdr_line.find(':');
        if (colon == std::string::npos) continue;
        auto hname = hdr_line.substr(0, colon);
        auto hval  = hdr_line.substr(colon + 1);
        while (!hval.empty() && hval[0] == ' ')
            hval.erase(0, 1);
        auto hnl = hname;
        for (auto& c : hnl)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (hnl == "content-length") {
            f.has_content_length = true;
            f.content_length = 0;
            for (char c : hval) {
                if (c >= '0' && c <= '9')
                    f.content_length = f.content_length * 10 + size_t(c - '0');
            }
        } else if (hnl == "transfer-encoding") {
            auto hvl = hval;
            for (auto& c : hvl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (hvl.find("chunked") != std::string::npos)
                f.chunked = true;
        } else if (hnl == "connection") {
            auto hvl = hval;
            for (auto& c : hvl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (hvl.find("close") != std::string::npos)
                f.upstream_close = true;
            if (hvl.find("keep-alive") != std::string::npos)
                f.upstream_keepalive = true;
        }

        if (hnl == "transfer-encoding" || hnl == "connection" ||
            hnl == "keep-alive" || hnl == "proxy-connection" ||
            hnl == "upgrade" || hnl == "content-length")
            continue;   // 只发一个 Content-Length，由调用方控制 framing
        f.headers.emplace_back(std::move(hnl), std::move(hval));
    }
}

// ── 构造 ──

// 构造：由上游地址列表自建连接池（owned_pool_），并绑定到内部池
// 参数：upstreams - 上游服务器地址列表
ReverseProxy::ReverseProxy(std::vector<UpstreamAddr> upstreams)
{
    std::vector<UpstreamServer> servers;
    servers.reserve(upstreams.size());
    for (auto& a : upstreams)
        servers.push_back({a.host, a.port});
    owned_pool_ = std::make_unique<UpstreamPool>(std::move(servers));
    pool_ = owned_pool_.get();
}

// 构造：绑定外部传入的共享上游池（不持有所有权）
// 参数：pool - 外部上游连接池引用
ReverseProxy::ReverseProxy(UpstreamPool& pool)
    : pool_(&pool) {}

// 同步路径不支持转发（需要异步 I/O），无池时返回 502，否则 502 兜底
// 参数：ctx - HTTP 请求上下文
Response ReverseProxy::Handle(const Context& ctx)
{
    if (!ctx.Pool()) return Response::Raw(502, R"({"error":"Bad Gateway"})");
    return Response::Error(502, *ctx.Pool());
}

// ═══════════════════════════════════════════════════════════════════
// HandleAsync — 选上游 → Forward → 上报健康状态
// ═══════════════════════════════════════════════════════════════════
// 处理异步转发：从池中选一个健康上游，转发请求并按状态码上报成功/失败
// 参数：ctx - HTTP 请求上下文；返回最终响应
coro::Task<Response> ReverseProxy::HandleAsync(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool) co_return Response::Raw(502, R"({"error":"Bad Gateway"})");

    // ── 选上游 ──
    const auto* upstream = pool_->Pick();
    if (!upstream) {
        std::cerr << "[proxy] 无可用上游" << std::endl;
        co_return Response::Error(502, *pool);
    }

    // ── 转发并组装响应 ──
    auto resp = co_await Forward(ctx, upstream->host, upstream->port);

    if (resp.StatusCode() >= 500) {
        pool_->ReportFailure(upstream);
    } else {
        pool_->ReportSuccess(upstream);
    }

    co_return resp;
}

// ═══════════════════════════════════════════════════════════════════
// Forward — 建连（含池复用）→ 发送请求 → 读响应 → 池归还
// ═══════════════════════════════════════════════════════════════════
// 转发单个请求到指定上游：优先复用池化连接，发送后解析 framing 读响应，
// 连接健康且可复用则归还连接池
// 参数：ctx - HTTP 请求上下文；host - 上游主机；port - 上游端口；返回组装好的响应
coro::Task<Response> ReverseProxy::Forward(
    const Context& ctx,
    std::string_view host,
    unsigned short port)
{
    auto* region = ctx.Pool();
    if (!region) co_return Response::Error(502, *region);

    auto& conn_pool = UpstreamConnPool::Instance();
    auto host_str   = std::string(host);
    auto port_str   = std::to_string(port);

    // ── 建连辅助：net::connect 内部 resolve + 非阻塞 connect ──
    auto make_conn = [&]() -> coro::Task<std::unique_ptr<net::TcpStream>> {
        return net::connect(host_str, port, 10000);
    };

    // ── 尝试取池化连接 ──
    auto pooled = conn_pool.Acquire(host_str, port);
    std::unique_ptr<net::TcpStream> sock;
    bool from_pool = false;

    if (pooled) {
        sock = std::make_unique<net::TcpStream>(std::move(pooled->socket));
        from_pool = true;
    }

    // 无池化连接时新建
    if (!sock) {
        sock = co_await make_conn();
        if (!sock) co_return Response::Error(502, *region);
    }

    // ── 组装上游请求（与上游保持 keep-alive）──
    std::string req;
    req.reserve(4096);

    req += ctx.Method();
    req += ' ';
    req += ctx.Path();
    req += " HTTP/1.1\r\n";

    req += "Host: ";
    req += host_str;
    if (port != 80 && port != 443) {
        req += ':';
        req += port_str;
    }
    req += "\r\n";

    // 转发请求头（跳过 hop-by-hop）
    auto hop_by_hop = [](std::string_view name) -> bool {
        return name == "host" || name == "connection"
            || name == "transfer-encoding" || name == "proxy-connection"
            || name == "keep-alive" || name == "upgrade";
    };

    for (int i = 0; i < ctx.HeaderCount(); i++) {
        auto [name, value] = ctx.HeaderAt(i);
        if (hop_by_hop(name)) continue;
        req += name;
        req += ": ";
        req += value;
        req += "\r\n";
    }

    // 请求上游保持 keep-alive
    req += "Connection: keep-alive\r\n";

    auto body = ctx.Body();
    if (!body.empty()) {
        req += "Content-Length: ";
        req += std::to_string(body.size());
        req += "\r\n";
    }

    req += "\r\n";

    // ── 发送请求（池化连接已失效时重试一次）──
    bool sent = co_await sock->write_all(req);

    // 池化连接可能已被上游关闭：重连并重发一次
    if (!sent && from_pool) {
        sock = co_await make_conn();
        if (sock) {
            from_pool = false;
            sent = co_await sock->write_all(req);
        }
    }

    // 发送 body（仅成功后）
    if (sent && !body.empty()) {
        if (!(co_await sock->write_all(body)))
            sent = false;
    }
    if (!sent) co_return Response::Error(502, *region);

    // ── 读响应（循环跳过 1xx 临时响应，直到非 1xx 最终响应）──
    net::BufferedReader reader(*sock);

    std::string status_line;
    std::string hdr_block;
    int status_code = 0;
    bool http10 = false;
    for (;;) {
        auto r1 = co_await reader.read_until("\r\n", status_line);
        if (!r1.ok()) co_return Response::Error(502, *region);

        {
            auto sp1 = status_line.find(' ');
            if (sp1 == std::string::npos) co_return Response::Error(502, *region);
            auto sp2 = status_line.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) co_return Response::Error(502, *region);
            status_code = std::atoi(status_line.c_str() + sp1 + 1);
        }
        // HTTP/1.0 无显式 Connection: keep-alive 时默认连接关闭，不能复用
        http10 = (status_line.rfind("HTTP/1.0", 0) == 0);

        auto r2 = co_await reader.read_until("\r\n\r\n", hdr_block);
        if (!r2.ok()) co_return Response::Error(502, *region);

        if (status_code >= 100 && status_code < 200) {
            // 1xx 临时响应（100 Continue / 103 Early Hints…）：丢弃头部，
            // 继续在同一连接上读最终响应（绝不能把 1xx 当最终响应）
            continue;
        }
        break;
    }

    // ── 解析头：Content-Length / Transfer-Encoding / Connection ──
    ResponseFraming fr;
    ParseResponseHeaders(hdr_block, fr);

    bool is_head = (ctx.Method() == "HEAD");
    bool no_body_status = (status_code == 204 || status_code == 304);

    // 3. Body（HEAD/204/304 无 body 优先；有 Content-Length 精确读；chunked 去帧；
    //    无 framing 仅在 Connection: close 时才读到 EOF，绝不挂 keep-alive）
    std::string body_buf;
    bool can_persist = false;   // 连接是否可安全复用

    if (is_head || no_body_status) {
        // HEAD/204/304 无 body：无论 framing 头如何都不读（RFC 7230 §4.3.2 允许
        // 它们带 Transfer-Encoding: chunked 但无 body，读取会挂死 keep-alive）
        can_persist = !fr.upstream_close;
    } else if (fr.chunked) {
        if (!(co_await ReadChunkedBody(reader, body_buf)))
            co_return Response::Error(502, *region);
        can_persist = !fr.upstream_close;
    } else if (fr.has_content_length) {
        // 有 Content-Length（含 0）：read_exact 精确读，不阻塞、不读到 EOF
        auto rb = co_await reader.read_exact(fr.content_length, body_buf);
        can_persist = rb.ok() && body_buf.size() == fr.content_length;
    } else {
        // 无 framing：只有上游明确 Connection: close（或 HTTP/1.0 语义）才读到 EOF；
        // keep-alive 且无 framing 无法确定边界，取已缓冲字节、不复用连接
        auto bv = reader.buffered();
        if (!bv.empty())
            body_buf.append(bv.data(), bv.size());
        if (fr.upstream_close) {
            std::array<char, 65536> read_buf;
            for (;;) {
                auto r = co_await sock->read_some(read_buf.data(), read_buf.size());
                if (!r.ok()) break;   // EOF 或错误
                body_buf.append(read_buf.data(), r.bytes);
            }
        }
        can_persist = false;
    }

    // 上游主动要求关闭则不复用；HTTP/1.0 无显式 keep-alive 亦默认关闭
    if (fr.upstream_close || (http10 && !fr.upstream_keepalive))
        can_persist = false;

    // ── 组装响应 ──
    Response resp(status_code, *region);

    for (auto& [hnl, hval] : fr.headers)
        resp.Header(hnl, hval);

    // 只发一个 Content-Length：
    //  - HEAD：转发上游声明的长度（描述 GET body）
    //  - 204/304：无 body，不发
    //  - 其他：实际 body 大小
    if (is_head) {
        resp.Header("content-length", std::to_string(fr.content_length));
    } else if (!no_body_status) {
        resp.Header("content-length", std::to_string(body_buf.size()));
    }
    resp.EndHeaders();

    if (!body_buf.empty())
        region->Write(std::string_view{body_buf.data(), body_buf.size()});

    // ── 连接健康且可复用则归还池 ──
    if (can_persist && sock->is_open() && status_code < 500)
    {
        auto returned = std::make_unique<UpstreamConnPool::Conn>();
        returned->socket = std::move(*sock);
        conn_pool.Release(std::move(returned), host_str, port);
    }

    co_return resp;
}

// ═══════════════════════════════════════════════════════════════════
// HandleWebSocket — WebSocket 透传上游（双向帧中继）
// ═══════════════════════════════════════════════════════════════════
// 处理 WebSocket 升级请求：向选中的上游建连并透传 upgrade 头，
// 之后双向中继帧（客户端→上游与上游→客户端两腿并发）
// 参数：ctx - HTTP 请求上下文；client_conn - 客户端 WebSocket 连接
coro::Task<void> ReverseProxy::HandleWebSocket(
    const Context& ctx, WsConnectionBase& client_conn)
{
    auto* upstream = pool_->Pick();
    if (!upstream) {
        std::cerr << "[proxy] WS 无可用上游" << std::endl;
        static const std::string_view k502 =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\n"
            "Content-Length: 13\r\nConnection: close\r\n\r\nBad Gateway\r\n";
        (void)co_await client_conn.WriteRaw(k502);
        co_return;
    }

    auto host_str = std::string(upstream->host);
    auto port_str = std::to_string(upstream->port);

    // ── 建连上游 ──
    auto upstream_sock = co_await net::connect(host_str, upstream->port, 10000);
    if (!upstream_sock) { pool_->ReportFailure(upstream); co_return; }

    // ── 转发 upgrade 请求（请求头原样透传，保留 Upgrade/Connection/Sec-WebSocket-*）──
    std::string req;
    req.reserve(4096);
    req += ctx.Method(); req += ' '; req += ctx.Path(); req += " HTTP/1.1\r\n";
    req += "Host: ";
    req += host_str;
    if (upstream->port != 80 && upstream->port != 443) {
        req += ':'; req += port_str;
    }
    req += "\r\n";

    for (int i = 0; i < ctx.HeaderCount(); i++) {
        auto [name, value] = ctx.HeaderAt(i);
        req += name; req += ": "; req += value; req += "\r\n";
    }
    req += "\r\n";

    if (!(co_await upstream_sock->write_all(req))) {
        pool_->ReportFailure(upstream);
        co_return;
    }

    // ── 读上游响应（状态行 + 头块），组装完整响应头 ──
    net::BufferedReader reader(*upstream_sock);
    std::string status_line;
    auto r1 = co_await reader.read_until("\r\n", status_line);
    if (!r1.ok()) { pool_->ReportFailure(upstream); co_return; }

    std::string hdr_block;
    auto r2 = co_await reader.read_until("\r\n\r\n", hdr_block);
    if (!r2.ok()) { pool_->ReportFailure(upstream); co_return; }

    std::string upstream_wire = status_line + "\r\n" + hdr_block + "\r\n\r\n";

    if (status_line.find("101") == std::string::npos) {
        // 上游拒绝升级：读取错误响应 body（按 framing），透传状态行 + 头 + body
        pool_->ReportFailure(upstream);
        ResponseFraming fr;
        ParseResponseHeaders(hdr_block, fr);
        std::string err_body;
        if (fr.chunked) {
            if (!(co_await ReadChunkedBody(reader, err_body))) { /* 忽略读错误 */ }
        } else if (fr.has_content_length) {
            auto rb = co_await reader.read_exact(fr.content_length, err_body);
            (void)rb;
        } else {
            auto bv = reader.buffered();
            if (!bv.empty()) err_body.append(bv.data(), bv.size());
            if (fr.upstream_close) {
                std::array<char, 65536> buf;
                for (;;) {
                    auto r = co_await upstream_sock->read_some(buf.data(), buf.size());
                    if (!r.ok()) break;
                    err_body.append(buf.data(), r.bytes);
                }
            }
        }
        std::string wire = status_line + "\r\n";
        for (auto& [hnl, hval] : fr.headers) {
            wire += hnl; wire += ": "; wire += hval; wire += "\r\n";
        }
        wire += "content-length: " + std::to_string(err_body.size()) + "\r\n\r\n";
        wire += err_body;
        (void)co_await client_conn.WriteRaw(wire);
        co_return;
    }
    pool_->ReportSuccess(upstream);

    // ── 把 101 响应透传给客户端 ──
    if (!(co_await client_conn.WriteRaw(upstream_wire))) co_return;

    // ── 双向帧中继 ──
    std::atomic<bool> relay_done{false};

    // 101 头块之后若已有上游字节（首帧），先喂给帧读取器再回落 socket
    FrameSource<net::TcpStream> upstream_src{
        std::string(reader.buffered()), 0, *upstream_sock};

    auto relay_c2u = [&]() -> coro::Task<void> {
        while (!relay_done.load(std::memory_order_relaxed)) {
            auto frame = co_await client_conn.Read();
            if (!client_conn.IsOpen()) {
                // 客户端连接结束（EOF/错误）→ 通知上游关闭，让上游腿收尾
                co_await WriteFrame(*upstream_sock, WsOpcode::Close, {},
                                    true, true);
                relay_done.store(true, std::memory_order_relaxed);
                break;
            }
            if (frame.opcode == WsOpcode::Close) {
                co_await WriteFrame(*upstream_sock, WsOpcode::Close,
                                    std::move(frame.payload), true, true);
                relay_done.store(true, std::memory_order_relaxed);
                break;
            }
            // 客户端帧已解掩码，转发时重新加掩码（mask=true，密钥 0 即恒等）
            co_await WriteFrame(*upstream_sock, frame.opcode,
                                std::move(frame.payload), frame.fin, true);
        }
        relay_done.store(true, std::memory_order_relaxed);
        co_return;
    };

    auto relay_u2c = [&]() -> coro::Task<void> {
        while (!relay_done.load(std::memory_order_relaxed)) {
            // ReadFrame 出错/超时/半读返回 nullopt → 上游连接结束
            auto opt = co_await ReadFrame(upstream_src);
            if (!opt.has_value()) break;
            auto frame = std::move(*opt);
            if (frame.opcode == WsOpcode::Close) {
                co_await client_conn.Close(
                    frame.payload.size() >= 2
                        ? static_cast<uint16_t>(
                              (static_cast<uint8_t>(frame.payload[0]) << 8) |
                              static_cast<uint8_t>(frame.payload[1]))
                        : 1000);
                relay_done.store(true, std::memory_order_relaxed);
                break;
            }
            co_await client_conn.Send(frame.opcode, std::move(frame.payload),
                                      frame.fin);
        }
        // 上游腿结束 → 关闭客户端连接，让客户端腿读到 EOF 收尾
        relay_done.store(true, std::memory_order_relaxed);
        co_await client_conn.Close();
        co_return;
    };

    // 两腿都是 coro::Task<void>，必须用 when_all_void 并发等待
    (void)co_await net::when_all_void(relay_c2u(), relay_u2c());
    relay_done.store(true, std::memory_order_relaxed);

    if (upstream_sock->is_open())
        upstream_sock->close();
    co_return;
}
