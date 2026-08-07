#include "server/server.hpp"
#include "server/h11_session.hpp"
#include "net/tls_stream.h"
#include "net/tls_context.h"
#include "coro/awaiter.h"
#include "log/logger.hpp"
#include <csignal>
#include <memory>

Server::Server(const Config& cfg,
               Router& router,
               MiddlewareManager& middleware,
               std::shared_ptr<net::TlsContext> tls)
    : ServerBase(cfg, router, middleware, std::move(tls)) {}

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

coro::Task<void> Server::Listen()
{
    while (!shutdown_) {
        net::TcpStream tcp;
        auto r = co_await listener_.accept(tcp);
        if (!r.ok()) {
            if (shutdown_) break;
            Logger::Log(LogLevel::Warn, "NET", "accept 失败，重试");
            co_await coro::sleep_for(10);
            continue;
        }
        if (shutdown_) { tcp.close(); break; }
        active_sessions_.fetch_add(1);
        coro::Task<void> h = (cfg_.tls_port > 0)
            ? HandleTls(std::move(tcp))
            : HandlePlain(std::move(tcp));
        loop_.post(h.handle());
    }
    Logger::Log(LogLevel::Info, "SERVER", "监听已停止");
    co_return;
}

coro::Task<void> Server::HandleTls(net::TcpStream tcp)
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

coro::Task<void> Server::HandlePlain(net::TcpStream tcp)
{
    auto session = std::make_shared<H11Session<net::TcpStream>>(
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
