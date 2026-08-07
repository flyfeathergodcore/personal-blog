// ═══════════════════════════════════════════════════════════════════
// demo_server — 基于 coro 新接口的演示服务器
//
// 用法：demo_server [-c config.yaml]
//   -c 配置文件路径（默认 config.yaml）
//
// 演示：H1 静态文件 / SSE / health / metrics / H1 WebSocket echo，
// 跑在 net 协程原语（TcpStream/TlsStream/TcpListener）之上。
// ═══════════════════════════════════════════════════════════════════
#include "config/config.hpp"
#include "log/logger.hpp"
#include "router/router.hpp"
#include "handler/request_handler.hpp"
#include "handler/health.hpp"
#include "handler/metrics.hpp"
#include "handler/reverse_proxy.hpp"
#include "handler/mysql_handler.hpp"
#include "handler/ws_echo.hpp"
#include "middleware/middleware.hpp"
#include "server/multi_server.hpp"
#include "server/ws_connection.hpp"
#include "net/tls_context.h"
#include "net/tcp_listener.h"
#include "net/buffered_reader.h"
#include "cache/file_cache.hpp"
#include "protocol/response.hpp"
#include "protocol/ws_frame.hpp"
#include "coro/awaiter.h"
#include <memory>
#include <string>
#include <thread>
#include <signal.h>
#include <cctype>
#include <cstring>

// ── SSE 示例 handler（演示 coro 流式接口）──
class SseHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override {
        // 实际路径走 HandleStream（IsStream()==true），此处仅兜底
        return Response::Error(404, *ctx.Pool());
    }
    bool IsStream() const override { return true; }
    coro::Task<void> HandleStream(const Context& /*ctx*/, StreamSink& sink) override {
        for (int i = 0; i < 10 && !sink.IsDisconnected(); ++i) {
            if (!(co_await sink.PushSSE("tick=" + std::to_string(i)))) break;
            co_await coro::sleep_for(100);
        }
        sink.End();
        co_return;
    }
};

// ═══════════════════════════════════════════════════════════════════
// WsEchoUpstream — 内部 WS echo 上游（供反向代理 WS 直连测试）
//
// plain TCP 监听 127.0.0.1:<port>，独立线程 + 事件循环。收到 WS upgrade
// 请求后回 101，随后用 WsConnection 回显每一帧（与 /ws 的 echo handler 同逻辑）。
// 仅作反向代理 WS 透传的验证上游，非正式服务。
// ═══════════════════════════════════════════════════════════════════
class WsEchoUpstream {
public:
    explicit WsEchoUpstream(uint16_t port) {
        if (!listener_.open("127.0.0.1", port, true)) {
            std::cerr << "[upstream] WS echo 上游监听失败: " << port << std::endl;
            return;
        }
        loop_ = std::make_unique<coro::EventLoop>();
        coro::Task<void> l = AcceptLoop();
        loop_->post(l.handle());
        thread_ = std::jthread([this] {
            // 该线程在 MultiServer 设置 signalfd 之前创建，SIGTERM/SIGINT 未阻塞；
            // 若不在此屏蔽，kill -TERM 会投递给本线程走默认处理，导致进程被 143 终止
            // 而非优雅关闭。这里屏蔽后，信号只会投递给已阻塞+signalfd 处理的 worker。
            sigset_t ss;
            sigemptyset(&ss);
            sigaddset(&ss, SIGINT);
            sigaddset(&ss, SIGTERM);
            pthread_sigmask(SIG_BLOCK, &ss, nullptr);
            loop_->run();
        });
        std::cout << "[upstream] WS echo 上游已启动 127.0.0.1:" << port << std::endl;
    }

    ~WsEchoUpstream() {
        if (loop_) loop_->stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    coro::Task<void> AcceptLoop() {
        for (;;) {
            net::TcpStream tcp;
            auto r = co_await listener_.accept(tcp);
            if (!r.ok()) {
                co_await coro::sleep_for(10);
                continue;
            }
            coro::Task<void> h = HandleClient(std::move(tcp));
            coro::EventLoop::current().post(h.handle());
        }
    }

    coro::Task<void> HandleClient(net::TcpStream tcp) {
        // 读 upgrade 请求头
        net::BufferedReader reader(tcp);
        std::string status_line, hdr_block;
        auto r1 = co_await reader.read_until("\r\n", status_line);
        if (!r1.ok()) co_return;
        auto r2 = co_await reader.read_until("\r\n\r\n", hdr_block);
        if (!r2.ok()) co_return;

        auto key = FindHeader(hdr_block, "sec-websocket-key");
        if (key.empty()) {
            static const std::string_view k400 =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            (void)co_await tcp.write_all(k400);
            co_return;
        }

        auto accept = ComputeWsAccept(key);
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (!(co_await tcp.write_all(resp))) co_return;

        // echo 帧
        WsConnection<net::TcpStream> ws(tcp);
        while (true) {
            auto frame = co_await ws.Read();
            if (frame.opcode == WsOpcode::Close) break;
            co_await ws.Send(frame.opcode, std::move(frame.payload), true);
        }
        co_return;
    }

    static std::string FindHeader(const std::string& block,
                                  const std::string& name) {
        size_t pos = 0;
        while (pos < block.size()) {
            auto cr = block.find("\r\n", pos);
            auto end = (cr == std::string::npos) ? block.size() : cr;
            auto line = block.substr(pos, end - pos);
            pos = (cr == std::string::npos) ? block.size() : cr + 2;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto ln = line.substr(0, colon);
            std::string lnl = ln;
            for (auto& c : lnl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lnl == name) {
                auto val = line.substr(colon + 1);
                while (!val.empty() && val[0] == ' ')
                    val.erase(0, 1);
                return val;
            }
        }
        return {};
    }

    net::TcpListener listener_;
    std::unique_ptr<coro::EventLoop> loop_;
    std::jthread thread_;
};

// ── /metrics.json 直接输出指标（MetricsMiddleware 也会拦截，双保险）──
class MetricsHandler : public RequestHandler {
public:
    explicit MetricsHandler(MetricsCollector* mc) : mc_(mc) {}
    Response Handle(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(200, mc_->RenderMetricsJson());
        auto json = mc_->RenderMetricsJson();
        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", json.size());
        resp.EndHeaders();
        pool->Write(json);
        return resp;
    }
private:
    MetricsCollector* mc_;
};

int main(int argc, char** argv)
{
    std::string config_path = "config.yaml";
    if (argc >= 3 && std::strcmp(argv[1], "-c") == 0)
        config_path = argv[2];

    auto cfg = Config::Load(config_path);
    Logger::Init(cfg.log_dir, cfg.log_level);

    // ── Router ──
    auto file_cache = std::make_unique<FileCache>();
    file_cache->LoadDirectory(cfg.doc_root);

    Router router;
    router.Add("/", std::make_unique<StaticFileHandler>(file_cache.get()));
    router.Add("/health", std::make_unique<HealthHandler>());
    router.Add("/old", std::make_unique<RedirectHandler>("/", 301));
    router.Add("/sse", std::make_unique<SseHandler>());
    router.Add("/ws", std::make_unique<WsEchoHandler>());

    // ── 博客 REST API（MySQL 数据源；config 未配置 mysql.database 则跳过）──
    if (!cfg.mysql.database.empty()) {
        std::cout << "[route] /api/* → MySQL 博客接口 (db="
                  << cfg.mysql.database << ")" << std::endl;
        // 局域网访问开关初始化：读 site_config 到内存（默认关闭），并取 HOST_LAN_IP
        InitLanStateFromDb(cfg.mysql);
        RegisterBlogRoutes(router, cfg.mysql);
    }

    // ── 内部 WS echo 上游（供反向代理 WS 透传验证，plain TCP 127.0.0.1:8081）──
    WsEchoUpstream ws_echo_upstream(8081);

    // ── Proxy 路由（从 config 的 proxy_routes 注册，上游如 127.0.0.1:8080）──
    for (auto& pr : cfg.proxy_routes) {
        std::cout << "[route] " << pr.prefix << " → ReverseProxy" << std::endl;
        router.Add(pr.prefix,
                   std::make_unique<ReverseProxy>(pr.upstreams));
    }

    // ── Metrics ──
    auto metrics = std::make_shared<MetricsCollector>(cfg.threads);
    router.Add("/metrics.json", std::make_unique<MetricsHandler>(metrics.get()));

    // ── Middleware（洋葱模型：CORS / RequestId / Logging / Metrics）──
    MiddlewareManager middleware;
    middleware.Add(std::make_unique<CORSMiddleware>());
    // 局域网访问拦截：关闭时拒绝局域网 IP Host 的请求（localhost 放行）
    middleware.Add(std::make_unique<LanGuardMiddleware>());
    middleware.Add(std::make_unique<RequestIdMiddleware>());
    middleware.Add(std::make_unique<LoggingMiddleware>());
    middleware.Add(std::make_unique<MetricsMiddleware>(metrics.get()));

    // ── TLS 上下文（net::TlsContext，新协程原语版）──
    auto tls = std::make_shared<net::TlsContext>();
    // TLS 可选：仅 tls_port > 0 时才要求证书（本项目走明文 h1，跳过）
    if (cfg.tls_port > 0 && !tls->Load(cfg.tls_cert, cfg.tls_key)) {
        Logger::Log(LogLevel::Error, "DEMO",
            "TLS 证书加载失败: " + cfg.tls_cert);
        Logger::StopAll();
        return 1;
    }

    MultiServer server(cfg, router, middleware, tls, metrics);

    // 访问统计落库：每 60s 聚合实时指标写 site_stats（后台仪表盘历史趋势数据源）。
    // 回调运行在 worker 0 的 event loop（thread_local 连接池即 worker0 的池）。
    if (!cfg.mysql.database.empty()) {
        server.SetPersistCallback([metrics, &cfg]() -> coro::Task<void> {
            co_await PersistSiteStats(metrics.get(), cfg.mysql);
        });
    }

    server.Start();

    Logger::StopAll();
    return 0;
}
