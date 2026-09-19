#include "http/server/server.hpp"
#include "http/server/h11_session.hpp"
#include "net/tls_stream.h"
#include "net/tls_context.h"
#include "coro/awaiter.h"
#include "log/logger.hpp"
#include <csignal>
#include <memory>

// 构造函数：保存配置、路由、中间件与 TLS 上下文
// 参数：cfg - 服务配置；router - 路由表；middleware - 中间件管理器；tls - TLS 上下文
Server::Server(const Config& cfg,
               Router& router,
               MiddlewareManager& middleware,
               std::shared_ptr<net::TlsContext> tls)
    : ServerBase(cfg, router, middleware, std::move(tls)) {}

// 启动单 worker 服务：接管信号、打开监听 socket、跑事件循环直至优雅关闭
// 参数：无
void Server::Start()
{
    ::signal(SIGPIPE, SIG_IGN);
    // 先接管信号再创建线程（与 MultiServer 同理：Logger writer 线程需继承屏蔽集）
    if (!sig_.init({SIGINT, SIGTERM}))
        Logger::Log(LogLevel::Error, "SERVER", "signalfd 初始化失败");
    if (!listener_.open(host_.c_str(), port_, false)) {
        Logger::Log(LogLevel::Error, "SERVER", "listener open 失败");
        return;
    }

    Logger::Log(LogLevel::Info, "SERVER",
        "监听 " + host_ + ":" + std::to_string(port_) + " (单 worker)");

    thread_ = std::jthread([this] { loop_.run(); });
    {
        coro::Task<void> l = Listen();
        loop_.post(l.handle());
        coro::Task<void> s = SignalLoop();
        loop_.post(s.handle());
    }
    thread_.join();
    Logger::Log(LogLevel::Info, "SERVER", "已完全停止");
}

// 监听协程：循环 accept 新连接并 post 到事件循环处理，直到 shutdown
// 参数：无
coro::Task<void> Server::Listen()
{
    while (!shutdown_) {
        tcp::Stream tcp;
        auto r = co_await listener_.accept(tcp);
        if (!r.ok()) {
            if (shutdown_) break;
            Logger::Log(LogLevel::Warn, "NET", "accept 失败，重试");
            co_await coro::sleep_for(10);
            continue;
        }
        if (shutdown_) { tcp.close(); break; }
        active_sessions_.fetch_add(1);
        if (cfg_.tls_port > 0) {
            auto h = HandleTls(std::move(tcp));
            loop_.post(h.handle());
        } else {
            auto h = HandlePlain(std::move(tcp));
            loop_.post(h.handle());
        }
    }
    Logger::Log(LogLevel::Info, "SERVER", "监听已停止");
    co_return;
}

// 处理 TLS 连接：握手后按 ALPN 选择 H1 会话（H2 未移植则回 426）并驱动 Start()
// 参数：tcp - 已 accept 的 TCP 流
coro::Task<void> Server::HandleTls(tcp::Stream tcp)
{
    net::TlsStream ss(std::move(tcp), tls_->NativeContext());
    auto hs = co_await ss.handshake(10000);
    if (!hs.ok()) {
        active_sessions_.fetch_sub(1);
        co_return;
    }
    if (net::TlsContext::IsHttp2(ss.native_handle())) {
        Logger::Log(LogLevel::Warn, "NET", "收到 h2 ALPN，但 H2 未移植");
        static const std::string_view k426 =
            "HTTP/1.1 426 Upgrade Required\r\n"
            "Content-Type: text/plain\r\nContent-Length: 33\r\nConnection: close\r\n"
            "\r\nHTTP/2 not ready. Use HTTP/1.1.\r\n";
        (void)co_await ss.write_all(k426);
        active_sessions_.fetch_sub(1);
        co_return;
    }
    auto session = std::make_shared<H11Session<net::TlsStream>>(
        std::move(ss), router_, middleware_);
    session->SetMaxBodySize(cfg_.max_body_size);
    session->SetWsIdleTimeout(cfg_.ws_idle_timeout);
    try {
        co_await session->Start();
    } catch (std::exception& e) {
        Logger::Log(LogLevel::Warn, "SERVER",
            std::string("会话异常: ") + e.what());
    }
    active_sessions_.fetch_sub(1);
    co_return;
}

// 处理明文 HTTP/1.1 连接：直接新建 H1 会话并驱动 Start()
// 参数：tcp - 已 accept 的 TCP 流
coro::Task<void> Server::HandlePlain(tcp::Stream tcp)
{
    auto session = std::make_shared<H11Session<tcp::Stream>>(
        std::move(tcp), router_, middleware_);
    session->SetMaxBodySize(cfg_.max_body_size);
    session->SetWsIdleTimeout(cfg_.ws_idle_timeout);
    try {
        co_await session->Start();
    } catch (std::exception& e) {
        Logger::Log(LogLevel::Warn, "SERVER",
            std::string("会话异常: ") + e.what());
    }
    active_sessions_.fetch_sub(1);
    co_return;
}

// 信号等待协程：收到 SIGINT/SIGTERM 后触发优雅关闭（等待会话排空）并停止事件循环
// 参数：无
coro::Task<void> Server::SignalLoop()
{
    for (;;) {
        int sig_no = co_await sig_.wait();
        if (sig_no < 0) continue;
        Logger::Log(LogLevel::Info, "SERVER",
            "收到信号 " + std::to_string(sig_no) + "，开始优雅关闭（最多 " +
            std::to_string(kDrainTimeoutSec) + " 秒）");
        if (shutdown_.exchange(true)) continue;
        listener_.close();
        // 等待活跃会话排空
        for (int i = 0; i < kDrainTimeoutSec; ++i) {
            co_await coro::sleep_for(1000);
            if (active_sessions_.load() == 0) break;
        }
        loop_.stop();
        break;
    }
    co_return;
}
