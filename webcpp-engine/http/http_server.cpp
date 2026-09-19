// ═══════════════════════════════════════════════════════════════════
// demo_server — 基于 coro 新接口的演示服务器
//
// 用法：demo_server [-c config.yaml]
//   -c 配置文件路径（默认 config.yaml）
//
// 演示：H1 静态文件 / SSE / health / metrics / H1 WebSocket echo，
// 跑在 tcp::Stream/tcp::Listener 与 net::TlsStream 协程原语之上。
// ═══════════════════════════════════════════════════════════════════
#include "http/config/config.hpp"
#include "log/logger.hpp"
#include "http/router/router.hpp"
#include "http/handler/request_handler.hpp"
#include "http/handler/health.hpp"
#include "http/handler/metrics.hpp"
#include "http/handler/reverse_proxy.hpp"
#include "http/handler/mysql_handler.hpp"
#include "http/handler/ws_echo.hpp"
#include "http/middleware/middleware.hpp"
#include "http/server/multi_server.hpp"
#include "net/tls_context.h"
#include "http/cache/file_cache.hpp"
#include "http/protocol/response.hpp"
#include "coro/awaiter.h"
#include <memory>
#include <string>
#include <cstring>
#include <chrono>

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
        co_await sink.End();
        co_return;
    }
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

    // 指标参数热配置装配：先装 yaml 默认值，再（MySQL 可用时）从
    // site_config(key='metrics_config') 读回后台在线覆盖值覆盖之。
    ApplyMetricsConfig(cfg.metrics);

    // ── Router ──
    auto file_cache = std::make_unique<FileCache>();
    file_cache->LoadDirectory(cfg.doc_root);
    // 启动后台缓存刷新：前端 dist 更新后无需重启容器，最多一个扫描周期
    // （默认 5 秒）即生效。mtime/大小变化、新增、删除都会被检测到。
    file_cache->StartRefresher(5);

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
        // 指标参数读回：后台在线覆盖值热生效（覆盖上面 yaml 默认值）
        InitMetricsConfigFromDb(cfg.mysql);
        RegisterBlogRoutes(router, cfg.mysql);
    }

    // ── Proxy 路由（从 config 的 proxy_routes 注册，上游如 127.0.0.1:8080）──
    for (auto& pr : cfg.proxy_routes) {
        std::cout << "[route] " << pr.prefix << " → ReverseProxy" << std::endl;
        router.Add(pr.prefix,
                   std::make_unique<ReverseProxy>(pr.upstreams));
    }

    // ── Metrics ──
    auto metrics = std::make_shared<MetricsCollector>(cfg.threads);
    router.Add("/metrics.json", std::make_unique<MetricsHandler>(metrics.get()));

    // ── Middleware（洋葱模型：CORS / LanGuard / VisitorTrack / RequestId / Logging / Metrics）──
    MiddlewareManager middleware;
    middleware.Add(std::make_unique<CORSMiddleware>());
    // 局域网访问拦截：关闭时拒绝局域网 IP Host 的请求（localhost 放行）
    middleware.Add(std::make_unique<LanGuardMiddleware>());
    // 访问者在线追踪：每请求刷新对端 IP 的最近活跃时间（在 LanGuard 之后，
    // 被 403 拦截的局域网请求不计数）
    middleware.Add(std::make_unique<VisitorTrackMiddleware>());
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
        // 清理周期判定改用 wall-clock（不再数落库次数）：落库周期在线可变后，
        // 按次数触发会跟着漂移；且清理检查放在 PersistSiteStats 之后无条件执行
        // ——无流量分钟落库内部 early-return 时清理也不会被饿死。
        auto last_cleanup = std::chrono::steady_clock::now();
        server.SetPersistCallback([metrics, &cfg, last_cleanup]() mutable -> coro::Task<void> {
            co_await PersistSiteStats(metrics.get(), cfg.mysql);
            // 每满 cleanup_interval_secs 清理一次 retention_days 天前的 site_stats，
            // 防止历史统计无限增长（两项均读运行时配置，后台可在线调整热生效）
            auto now = std::chrono::steady_clock::now();
            if (now - last_cleanup >=
                std::chrono::seconds(g_metrics_cfg.cleanup_interval_secs.load())) {
                last_cleanup = now;
                co_await CleanupSiteStats(cfg.mysql,
                                          g_metrics_cfg.cleanup_retention_days.load());
            }
        });
    }

    server.Start();

    Logger::StopAll();
    return 0;
}
