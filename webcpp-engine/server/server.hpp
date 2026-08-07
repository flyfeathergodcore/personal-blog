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
    Server(const Config& cfg,
           Router& router,
           MiddlewareManager& middleware,
           std::shared_ptr<net::TlsContext> tls);

    void Start() override;

private:
    coro::Task<void> Listen();
    coro::Task<void> HandleTls(net::TcpStream tcp);
    coro::Task<void> HandlePlain(net::TcpStream tcp);
    coro::Task<void> SignalLoop();

    coro::EventLoop loop_;
    net::TcpListener listener_;
    net::SignalWatcher sig_;
    std::jthread thread_;
    std::atomic<size_t> active_sessions_{0};
};
