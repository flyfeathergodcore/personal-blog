#include "server/multi_server.hpp"
#include "server/h2_session.hpp"
#include "coro/awaiter.h"
#include "log/logger.hpp"
#include <csignal>
#include <unistd.h>

// 构造函数：保存配置、路由、中间件、TLS 上下文与指标收集器
// 参数：cfg - 服务配置；router - 路由表；middleware - 中间件管理器；tls - TLS 上下文；metrics - 指标收集器
MultiServer::MultiServer(const Config& cfg,
                         Router& router,
                         MiddlewareManager& middleware,
                         std::shared_ptr<net::TlsContext> tls,
                         std::shared_ptr<MetricsCollector> metrics)
    : ServerBase(cfg, router, middleware, std::move(tls))
    , metrics_(std::move(metrics)) {}

// 启动多 worker 服务：校验配置、接管信号、创建 worker 线程（SO_REUSEPORT 监听）、
// 挂载监听/刷指标/信号等待协程，最后 join 等待优雅关闭
// 参数：无
void MultiServer::Start()
{
    // ── 防御：worker 线程数非法（<1）时拒绝启动 ──
    // 正常路径由 Config::Load 钳制到 ≥1，这里作为兜底，避免 workers_ 为空时
    // 后面 workers_[0] 越界访问（UB）。正常起始状态下 workers_ 为空，故检查 cfg_。
    if (cfg_.threads < 1) {
        Logger::Log(LogLevel::Error, "SERVER",
            "threads 配置非法（" + std::to_string(cfg_.threads) + "），拒绝启动");
        return;
    }

    ::signal(SIGPIPE, SIG_IGN);

    // 先接管信号（signalfd + block），再创建任何线程。
    // 顺序关键：Logger::Log 会惰性创建 writer 线程；若在 block 之前写日志，
    // 该线程不继承屏蔽集，SIGTERM/SIGINT 会直接杀死进程而不是进 signalfd。
    net::SignalWatcher sig;
    if (!sig.init({SIGINT, SIGTERM}))
        Logger::Log(LogLevel::Error, "SERVER", "signalfd 初始化失败");

    auto port = port_;
    Logger::Log(LogLevel::Info, "SERVER",
        "监听 " + host_ + ":" + std::to_string(port) + " (" +
        std::to_string(cfg_.threads) + " workers, SO_REUSEPORT)");

    workers_.reserve(cfg_.threads);
    for (int i = 0; i < cfg_.threads; ++i) {
        auto w = std::make_unique<Worker>();
        // TcpListener::open 内部：socket() → setsockopt(SO_REUSEPORT|SO_REUSEADDR) → bind → listen
        // （SO_REUSEPORT 在 open 内设置，Task 4 已实现）
        if (!w->listener.open(host_.c_str(), port, true))
            Logger::Log(LogLevel::Error, "SERVER", "listener open 失败");
        w->thread = std::jthread([w = w.get()] { w->loop.run(); });
        // 主线程显式 post（不得用 current()）
        {
            coro::Task<void> l = Listen(*w, i);
            w->loop.post(l.handle());
            coro::Task<void> f = FlushLoop(i);
            w->loop.post(f.handle());
        }
        workers_.push_back(std::move(w));
    }
    // ── 防御：worker 创建循环后仍无 worker（理论上被上面 cfg_.threads 挡下）──
    // 在索引 workers_[0] 之前再兜底一次，防止越界访问。
    if (workers_.empty()) {
        Logger::Log(LogLevel::Error, "SERVER", "没有可用 worker，拒绝启动");
        return;
    }

    // 信号等待协程跑在 worker0 的 loop 上
    {
        // 立即调用的 lambda 协程闭包是临时对象，协程挂起（等信号）后闭包销毁
        // → this/捕获悬垂（GCC 侥幸、clang 必崩）；提为成员协程函数 WatchSignals，
        // this 指向 MultiServer（Start 存活到 join），sig 按引用传入（同存活）。
        coro::Task<void> s = WatchSignals(sig);
        workers_[0]->loop.post(s.handle());
    }
    for (auto& w : workers_) w->thread.join();
    Logger::Log(LogLevel::Info, "SERVER", "已完全停止");
}

// 信号等待协程：挂在 signalfd 上，收到 SIGINT/SIGTERM 触发优雅关闭。
// 原实现是立即调用的 lambda 协程，闭包临时对象在赋值语句结束后销毁，
// 协程（while true 长期挂起）恢复时 this/捕获悬垂——GCC 因实现细节恰好
// 不崩，clang 严格按标准会 use-after-scope 崩溃。成员协程 this 指向
// MultiServer（Start 存活到 join），sig 按引用进帧（Start 局部变量，同存活）。
// 参数：sig - 信号监视器（引用，与 Start 局部变量同存活）
coro::Task<void> MultiServer::WatchSignals(net::SignalWatcher& sig)
{
    while (true) {
        int sig_no = co_await sig.wait();
        if (sig_no < 0) continue;
        Logger::Log(LogLevel::Info, "SERVER",
            "收到信号 " + std::to_string(sig_no) + "，开始优雅关闭（最多 " +
            std::to_string(kDrainTimeoutSec) + " 秒）");
        if (shutdown_.exchange(true)) continue;
        for (auto& w : workers_) w->listener.close();   // 停止新连接
        for (auto& w : workers_) {
            coro::Task<void> d = DrainLoop(*w);
            w->loop.post(d.handle());
        }
        break;
    }
    co_return;
}

// 监听协程：循环 accept 新连接并 post 到对应 worker 事件循环处理，直到 shutdown
// 参数：w - 所属 worker；wid - worker 序号
coro::Task<void> MultiServer::Listen(Worker& w, int wid)
{
    while (!shutdown_) {
        net::TcpStream tcp;
        auto r = co_await w.listener.accept(tcp);
        if (!r.ok()) {
            if (shutdown_) break;
            Logger::Log(LogLevel::Warn, "NET", "accept 失败，重试");
            co_await coro::sleep_for(10);
            continue;
        }
        if (shutdown_) { tcp.close(); break; }
        w.active_sessions.fetch_add(1);
        coro::Task<void> h = (cfg_.tls_port > 0)
            ? HandleTls(w, wid, std::move(tcp))
            : HandlePlain(w, wid, std::move(tcp));
        w.loop.post(h.handle());   // 显式传 loop
    }
    Logger::Log(LogLevel::Info, "SERVER", "监听已停止");
    co_return;
}

// 处理 TLS 连接：握手后按 ALPN 选择 H2 会话或 H1 会话（H1 复用池外壳）并驱动 Start()
// 参数：w - 所属 worker；wid - worker 序号；tcp - 已 accept 的 TCP 流
coro::Task<void> MultiServer::HandleTls(Worker& w, int wid, net::TcpStream tcp)
{
    net::TlsStream ss(std::move(tcp), tls_->NativeContext());
    auto hs = co_await ss.handshake(10000);
    if (!hs.ok()) {
        w.active_sessions.fetch_sub(1);
        co_return;
    }
    if (net::TlsContext::IsHttp2(ss.native_handle())) {
        // H2（RFC 7540 over TLS/ALPN）：接入 coro/net 原语
        auto session = std::make_shared<H2Session>(
            std::move(ss), router_, middleware_, &w.region_pool);
        session->SetMaxBodySize(cfg_.max_body_size);
        session->SetWsIdleTimeout(cfg_.ws_idle_timeout);
        session->SetMetrics(metrics_.get(), wid);
        try {
            co_await session->Start();
        } catch (std::exception& e) {
            Logger::Log(LogLevel::Warn, "SERVER",
                std::string("h2 会话异常: ") + e.what());
        }
        w.active_sessions.fetch_sub(1);
        co_return;
    }
    // H1：复用池外壳
    auto session = w.pool.TryAcquireSession();
    if (!session) {
        session = std::make_shared<H11Session<net::TlsStream>>(
            std::move(ss), router_, middleware_, &w.region_pool);
    } else {
        session->Reset(std::move(ss));
        session->Region().Init(&w.region_pool);
    }
    session->SetMaxBodySize(cfg_.max_body_size);
    session->SetWsIdleTimeout(cfg_.ws_idle_timeout);
    session->SetMetrics(metrics_.get(), wid);
    try {
        co_await session->Start();
    } catch (std::exception& e) {
        Logger::Log(LogLevel::Warn, "SERVER",
            std::string("会话异常: ") + e.what());
    }
    w.pool.ReleaseSession(std::move(session));
    w.active_sessions.fetch_sub(1);
    co_return;
}

// 处理明文 HTTP/1.1 连接：直接新建 H1 会话并驱动 Start()
// 参数：w - 所属 worker；wid - worker 序号；tcp - 已 accept 的 TCP 流
coro::Task<void> MultiServer::HandlePlain(Worker& w, int wid, net::TcpStream tcp)
{
    // 非 TLS 路径（cfg_.tls_port == 0）：池只服务 TLS 类型，此路径直接新建 session，不复用外壳
    auto session = std::make_shared<H11Session<net::TcpStream>>(
        std::move(tcp), router_, middleware_, &w.region_pool);
    session->SetMaxBodySize(cfg_.max_body_size);
    session->SetWsIdleTimeout(cfg_.ws_idle_timeout);
    session->SetMetrics(metrics_.get(), wid);
    try {
        co_await session->Start();
    } catch (std::exception& e) {
        Logger::Log(LogLevel::Warn, "SERVER",
            std::string("会话异常: ") + e.what());
    }
    w.active_sessions.fetch_sub(1);
    co_return;
}

// 周期任务协程：每秒刷一次指标，worker 0 每 60s 执行一次落库回调
// 参数：worker_id - worker 序号
coro::Task<void> MultiServer::FlushLoop(int worker_id)
{
    int tick = 0;
    while (!shutdown_) {
        co_await coro::sleep_for(1000);
        if (shutdown_) break;
        metrics_->Flush(worker_id);
        // 访问统计落库：worker 0 每 60s 聚合一次写 site_stats（分钟粒度历史）
        if (worker_id == 0 && ++tick % 60 == 0 && persist_cb_) {
            co_await persist_cb_();
        }
    }
    co_return;
}

// 优雅关闭排水协程：每秒检查活跃会话，清零则 stop loop，超时强制 stop
// 参数：w - 待排水的 worker
coro::Task<void> MultiServer::DrainLoop(Worker& w)
{
    // 每 1s 检查活跃会话，清零则 stop；超过 kDrainTimeoutSec 强制 stop
    for (int i = 0; i < kDrainTimeoutSec; ++i) {
        co_await coro::sleep_for(1000);
        if (w.active_sessions.load() == 0) { w.loop.stop(); co_return; }
    }
    Logger::Log(LogLevel::Warn, "SERVER", "排水超时，强制关闭所有连接");
    w.loop.stop();
    co_return;
}
