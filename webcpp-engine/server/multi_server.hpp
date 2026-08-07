#pragma once
#include "server/server_base.hpp"
#include "server/h11_session.hpp"
#include "server/h11_session_pool.hpp"
#include "protocol/region_pool.hpp"
#include "handler/metrics.hpp"
#include "net/tcp_listener.h"
#include "net/tls_stream.h"
#include "net/signal_watcher.h"
#include "coro/event_loop.h"
#include <thread>
#include <vector>
#include <atomic>
#include <functional>

class MultiServer : public ServerBase {
public:
    MultiServer(const Config& cfg, Router& router, MiddlewareManager& middleware,
                std::shared_ptr<net::TlsContext> tls,
                std::shared_ptr<MetricsCollector> metrics);
    void Start() override;

    // 每 60s（worker 0）调用的落库回调：由 demo_server 注入，聚合实时指标写 site_stats
    void SetPersistCallback(std::function<coro::Task<void>()> cb) { persist_cb_ = std::move(cb); }

private:
    struct Worker {
        coro::EventLoop loop;
        net::TcpListener listener;
        H11SessionPool pool;
        RegionPool region_pool;
        std::jthread thread;
        std::atomic<size_t> active_sessions{0};
    };
    coro::Task<void> Listen(Worker& w, int wid);
    coro::Task<void> HandleTls(Worker& w, int wid, net::TcpStream tcp);
    coro::Task<void> HandlePlain(Worker& w, int wid, net::TcpStream tcp);   // SO_REUSEPORT 已在 open() 内
    coro::Task<void> FlushLoop(int worker_id);
    coro::Task<void> DrainLoop(Worker& w);
    std::vector<std::unique_ptr<Worker>> workers_;
    std::shared_ptr<MetricsCollector> metrics_;
    std::function<coro::Task<void>()> persist_cb_;   // 空则不落库
};
