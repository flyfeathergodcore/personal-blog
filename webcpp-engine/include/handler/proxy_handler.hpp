#pragma once
#include "handler/request_handler.hpp"
#include "coro/task.h"
#include <string>

// ═══════════════════════════════════════════════════════════════════
// ProxyHandler — 反向代理到单个上游 HTTP 服务器（coro/net 版）
//
// 把入站 HTTP/1.1 请求转发给配置的上游，读回响应并返回给调用方。
//
// 走 HandleAsync() 做非阻塞上游 I/O；连接用 net::connect 在调用协程
// 所在 worker 事件循环上建立，无需外部事件循环句柄。
// ═══════════════════════════════════════════════════════════════════

struct UpstreamConfig {
    std::string host;       // e.g. "127.0.0.1"
    unsigned short port;    // e.g. 3000
};

class ProxyHandler : public RequestHandler {
public:
    // 构造单上游代理
    // 参数：upstream - 上游地址配置（主机 + 端口）
    explicit ProxyHandler(UpstreamConfig upstream);

    // 同步路径（仅为兜底，实际走异步）
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override;
    // 异步转发请求到单个上游并返回响应
    // 参数：ctx - 请求上下文
    coro::Task<Response> HandleAsync(const Context& ctx) override;
    // 代理为 I/O 密集，走异步路径，始终返回 true
    bool IsAsync() const override { return true; }

private:
    UpstreamConfig upstream_;
};
