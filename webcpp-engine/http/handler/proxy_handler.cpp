#include "http/handler/proxy_handler.hpp"
#include "http/protocol/response.hpp"
#include "http/protocol/session_region.hpp"
#include "http/protocol/context.hpp"
#include "tcp/connector.hpp"
#include <array>
#include <cctype>
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

// 构造：保存单一上游的转发配置
// 参数：upstream - 上游地址与端口配置
ProxyHandler::ProxyHandler(UpstreamConfig upstream)
    : upstream_(std::move(upstream)) {}

// 同步路径不支持——需要 I/O，统一返回 502
// 参数：ctx - HTTP 请求上下文
Response ProxyHandler::Handle(const Context& ctx)
{
    // 同步路径不支持——需要 I/O。
    return Response::Error(502, *ctx.Pool());
}

// ═══════════════════════════════════════════════════════════════════
// HandleAsync — 建连 → 发送（Connection: close）→ 读响应（读到 EOF）
// ═══════════════════════════════════════════════════════════════════
// 处理异步转发：向单一上游建连并发送请求，声明 Connection: close，
// 读到 EOF 取回完整响应体后组装响应
// 参数：ctx - HTTP 请求上下文；返回组装好的响应
coro::Task<Response> ProxyHandler::HandleAsync(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool) co_return Response::Raw(502, R"({"error":"Bad Gateway"})");

    // ── 解析 + 建连 ──
    auto sock = co_await tcp::Connect(upstream_.host, upstream_.port, 10000);
    if (!sock) co_return Response::Error(502, *pool);

    // ── 组装上游请求 ──
    std::string req;
    req.reserve(4096);

    req += ctx.Method();
    req += ' ';
    req += ctx.Path();
    req += " HTTP/1.1\r\n";

    req += "Host: ";
    req += upstream_.host;
    req += ':';
    req += std::to_string(upstream_.port);
    req += "\r\n";

    // 转发入站请求头（跳过 hop-by-hop）
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

    // Content-Length
    auto body = ctx.Body();
    if (!body.empty()) {
        req += "Content-Length: ";
        req += std::to_string(body.size());
        req += "\r\n";
    }

    // 与上游用 Connection: close，读完即断（不复用连接）
    req += "Connection: close\r\n";
    req += "\r\n";

    // ── 发送 ──
    bool sent = co_await sock->write_all(req);
    if (sent && !body.empty())
        sent = co_await sock->write_all(body);
    if (!sent) co_return Response::Error(502, *pool);

    // ── 读响应 ──
    // 1. 状态行
    std::string status_line;
    auto r1 = co_await sock->read_until("\r\n", status_line);
    if (!r1.ok()) co_return Response::Error(502, *pool);

    int status_code = 0;
    {
        auto sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) co_return Response::Error(502, *pool);
        auto sp2 = status_line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) co_return Response::Error(502, *pool);
        status_code = std::atoi(status_line.c_str() + sp1 + 1);
    }

    // 2. 头块
    std::string hdr_block;
    auto r2 = co_await sock->read_until("\r\n\r\n", hdr_block);
    if (!r2.ok()) co_return Response::Error(502, *pool);

    // 组装响应
    Response resp(status_code, *pool);

    // 转发上游响应头（跳过 hop-by-hop；统一小写，兼容 H2）
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

        auto hname_lower = hname;
        for (auto& c : hname_lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (hname_lower == "transfer-encoding" || hname_lower == "connection" ||
            hname_lower == "keep-alive" || hname_lower == "proxy-connection" ||
            hname_lower == "upgrade")
            continue;

        // 统一小写（HTTP/2 必需，HTTP/1.1 无害）
        resp.Header(hname_lower, hval);
    }

    // 3. Body（读到 EOF —— Connection: close）
    std::string body_buf;
    body_buf.reserve(65536);

    {
        std::array<char, 65536> read_buf;
        for (;;) {
            auto r = co_await sock->read_some(read_buf.data(), read_buf.size());
            if (!r.ok()) break;   // EOF 或错误
            body_buf.append(read_buf.data(), r.bytes);
        }
    }

    resp.Header("Content-Length", std::to_string(body_buf.size()));
    resp.EndHeaders();

    // 写到 region（不能写 resp.Body() —— body_buf 是本协程帧的局部变量，
    // 挂到 region 上才与 Response 生命周期一致）
    if (!body_buf.empty())
        pool->Write(std::string_view{body_buf.data(), body_buf.size()});

    co_return resp;
}
