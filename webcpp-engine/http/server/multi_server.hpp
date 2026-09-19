#pragma once
#include "http/server/server_base.hpp"
#include "http/server/h11_session.hpp"
#include "http/server/h11_session_pool.hpp"
#include "http/protocol/region_pool.hpp"
#include "http/handler/metrics.hpp"
#include "tcp/listener.hpp"
#include "net/tls_stream.h"
#include "net/signal_watcher.h"
#include "coro/event_loop.h"
#include <thread>
#include <vector>
#include <atomic>
#include <functional>

class MultiServer : public ServerBase {
public:
    // 构造函数：保存配置、路由、中间件、TLS 上下文与指标收集器
    // 参数：cfg - 服务配置；router - 路由表；middleware - 中间件管理器；tls - TLS 上下文；metrics - 指标收集器
    MultiServer(const Config& cfg, Router& router, MiddlewareManager& middleware,
                std::shared_ptr<net::TlsContext> tls,
                std::shared_ptr<MetricsCollector> metrics);
    // 启动多 worker 服务（覆盖基类纯虚函数）
    // 参数：无
    void Start() override;

    // 每 60s（worker 0）调用的落库回调：由 demo_server 注入，聚合实时指标写 site_stats
    // 参数：cb - 落库回调（协程任务工厂）
    void SetPersistCallback(std::function<coro::Task<void>()> cb) { persist_cb_ = std::move(cb); }

private:
    struct Worker {
        coro::EventLoop loop;
        tcp::Listener listener;
        H11SessionPool pool;
        RegionPool region_pool;
        std::jthread thread;
        std::atomic<size_t> active_sessions{0};
    };
    // 监听协程：循环 accept 并派发连接
    // 参数：w - worker；wid - worker 序号
    coro::Task<void> Listen(Worker& w, int wid);
    // 处理 TLS 连接（握手后按 ALPN 分流 H1/H2）
    // 参数：w - worker；wid - worker 序号；tcp - TCP 流
    coro::Task<void> HandleTls(Worker& w, int wid, tcp::Stream tcp);
    // 处理明文 HTTP/1.1 连接
    // 参数：w - worker；wid - worker 序号；tcp - TCP 流
    coro::Task<void> HandlePlain(Worker& w, int wid, tcp::Stream tcp);   // SO_REUSEPORT 已在 open() 内
    // 周期任务协程：刷指标 + 定时落库
    // 参数：worker_id - worker 序号
    coro::Task<void> FlushLoop(int worker_id);
    // 优雅关闭排水协程
    // 参数：w - 待排水的 worker
    coro::Task<void> DrainLoop(Worker& w);
    // 信号等待协程（成员协程而非 lambda，避免闭包悬垂）——clang 兼容
    coro::Task<void> WatchSignals(net::SignalWatcher& sig);
    std::vector<std::unique_ptr<Worker>> workers_;
    std::shared_ptr<MetricsCollector> metrics_;
    std::function<coro::Task<void>()> persist_cb_;   // 空则不落库
};
