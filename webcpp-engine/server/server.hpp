#pragma once
#include "server/server_base.hpp"
#include "net/tcp_listener.h"
#include "net/signal_watcher.h"
#include "coro/event_loop.h"
#include <thread>
#include <atomic>

// 单 loop 的简化 Server（等价于 threads=1 的 MultiServer）。
// 主要用于编译可用性；正式演示走 MultiServer。
class Server : public ServerBase {
public:
    // 构造函数：保存配置、路由、中间件与 TLS 上下文
    // 参数：cfg - 服务配置；router - 路由表；middleware - 中间件管理器；tls - TLS 上下文
    Server(const Config& cfg,
           Router& router,
           MiddlewareManager& middleware,
           std::shared_ptr<net::TlsContext> tls);

    // 启动单 worker 服务（覆盖基类纯虚函数）
    // 参数：无
    void Start() override;

private:
    // 监听协程：循环 accept 并派发连接
    // 参数：无
    coro::Task<void> Listen();
    // 处理 TLS 连接（H2 未移植则回 426）
    // 参数：tcp - TCP 流
    coro::Task<void> HandleTls(net::TcpStream tcp);
    // 处理明文 HTTP/1.1 连接
    // 参数：tcp - TCP 流
    coro::Task<void> HandlePlain(net::TcpStream tcp);
    // 信号等待协程：触发优雅关闭
    // 参数：无
    coro::Task<void> SignalLoop();

    coro::EventLoop loop_;
    net::TcpListener listener_;
    net::SignalWatcher sig_;
    std::jthread thread_;
    std::atomic<size_t> active_sessions_{0};
};
