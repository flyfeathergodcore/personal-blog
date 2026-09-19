#pragma once
#include "http/handler/request_handler.hpp"
#include "http/handler/upstream_pool.hpp"
#include "http/config/config.hpp"
#include "coro/task.h"
#include <memory>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════
// ReverseProxy — HTTP/1.1 反向代理（带负载均衡）
//
// 把入站请求转发给从上游池选出的后端，读回响应并返回给调用方。
//
// 支持：
//   - 轮询负载均衡（UpstreamPool）
//   - 被动健康检查（失败计数 + 自动挂起）
//   - 请求头转发（统一小写，兼容 H2）
//   - 与上游保持 Connection: keep-alive 并复用连接（UpstreamConnPool）
//   - WebSocket 双向透传
// ═══════════════════════════════════════════════════════════════════

class ReverseProxy : public RequestHandler {
public:
    /// 从配置的上游地址构造（常见场景）。
    explicit ReverseProxy(std::vector<UpstreamAddr> upstreams);

    /// 从预建池构造（高级用法）。
    explicit ReverseProxy(UpstreamPool& pool);

    // 同步路径（反向代理为 I/O 密集，仅做兜底）
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override;
    // 异步转发请求到后端并返回响应
    // 参数：ctx - 请求上下文
    coro::Task<Response> HandleAsync(const Context& ctx) override;
    // 反向代理走异步路径，始终返回 true
    bool IsAsync() const override { return true; }

    /// 反向代理直接处理 WS upgrade：会话把 WS 升级请求直接路由到
    /// HandleWebSocket（不经过 HandleAsync/Forward，后者无法产生 101）。
    bool IsWebSocketUpgradeHandler() const override { return true; }

    /// WebSocket 透传 —— 升级到上游并中继帧
    coro::Task<void> HandleWebSocket(const Context& ctx,
                                     WsConnectionBase& client_conn) override;

private:
    // 向指定上游发送请求并读回响应
    // 参数：ctx - 请求上下文；host - 上游主机；port - 上游端口
    coro::Task<Response> Forward(const Context& ctx,
                                 std::string_view host,
                                 unsigned short port);

    std::unique_ptr<UpstreamPool> owned_pool_;  // 持有池（从地址构造时）
    UpstreamPool* pool_ = nullptr;              // 非拥有引用
};
