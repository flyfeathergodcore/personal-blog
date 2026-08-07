// ═══════════════════════════════════════════════════════════════════
// routes_demo — 如何添加路由与处理逻辑
//
// 用法：routes_demo [-c config.yaml]    （默认 config.yaml）
//
// 本示例不是完整服务器，而是"接线示范"：展示给 webcpp-engine
// 添加一个 handler 需要写什么、怎么注册、能拿到哪些请求数据。
//
//   ├─ 同步 handler（CPU 型，默认路径）         —— HelloHandler / EchoBodyHandler
//   ├─ 异步 handler（I/O 型，IsAsync=true）     —— SlowHandler
//   ├─ 路径参数 :param（/users/:id）            —— UserHandler（ctx.Param）
//   ├─ 通配 *（/files/*）                       —— FileInfoHandler（ctx.Param）
//   ├─ lambda 函数式注册                       —— /greet（无需写 handler 类）
//   ├─ SSE 流式（IsStream=true）                —— SseHandler
//   ├─ WebSocket upgrade（IsWebSocketUpgradeHandler=true）—— WsEchoHandler
//   └─ 兜底 /（未匹配路径返回 404）
//
// 地址拆解全部由 Router 引擎自动完成：query string 自动剥离匹配，
// :id / * 参数自动捕获注入 ctx（ctx.Param），query 参数自动解析（ctx.Query）。
// handler 内无需再解析 Path。编译后的二进制与 demo_server 同等接入。
// ═══════════════════════════════════════════════════════════════════
#include "config/config.hpp"
#include "log/logger.hpp"
#include "router/router.hpp"
#include "handler/request_handler.hpp"
#include "handler/ws_echo.hpp"
#include "middleware/middleware.hpp"
#include "server/multi_server.hpp"
#include "net/tls_context.h"
#include "cache/file_cache.hpp"
#include "protocol/response.hpp"
#include "protocol/context.hpp"
#include "protocol/session_region.hpp"
#include "coro/task.h"
#include "coro/awaiter.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <cstdio>

// ═══════════════════════════════════════════════════════════════════
// 1. 同步 handler —— 最常用，直接返回 Response。
//    Handle() 内禁止长时间阻塞（它会占用 worker 事件循环线程）。
// ═══════════════════════════════════════════════════════════════════

class HelloHandler : public RequestHandler {
public:
    // GET /hello?name=xxx → {"hello":"xxx"}（缺省 "world"）
    // ctx.Query() 由引擎从 ctx.Path() 自动解析 query string
    Response Handle(const Context& ctx) override {
        std::string_view name = ctx.Query("name");
        if (name.empty()) name = "world";

        // 读请求头（大小写不敏感）
        std::string_view ua = ctx.Header("user-agent");

        std::string body = R"({"hello":")";
        body += std::string(name);
        body += R"(","ua":")";
        body += (ua.empty() ? std::string("unknown") : std::string(ua));
        body += R"(","h2":)";
        body += ctx.IsHttp2() ? "true" : "false";
        body += "}";

        // 两种写法任选：
        //  (a) Raw —— 整个 wire 自己拼（简单但无 Content-Length 自动计算）
        //  return Response::Raw(200, "HTTP/1.1 200 OK\r\n...");
        //
        //  (b) region 版（推荐）—— 引擎序列化状态行 + 头 + body
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(500, "no pool");
        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write({body.data(), body.size()});
        return resp;
    }
};

// ═══════════════════════════════════════════════════════════════════
// 2. POST + body —— 读请求体做处理。
// ═══════════════════════════════════════════════════════════════════

class EchoBodyHandler : public RequestHandler {
public:
    // POST /echo 回显收到的 body
    Response Handle(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(500, "no pool");

        std::string_view body = ctx.Body();
        // ctx.Body() 指向解析缓冲；如需改造成可写 string 再复制
        std::string reply = "echo(" + std::to_string(body.size()) + "B): ";
        reply.append(body.data(), body.size());

        Response resp(200, *pool);
        resp.Header("Content-Type", "text/plain");
        resp.Header("Content-Length", reply.size());
        resp.EndHeaders();
        pool->Write({reply.data(), reply.size()});
        return resp;
    }
};

// ═══════════════════════════════════════════════════════════════════
// 3. 异步 handler —— 协程路径，适合 I/O（代理、上游调用、长操作）。
//    IsAsync() 返回 true，session 走 HandleAsync()。
// ═══════════════════════════════════════════════════════════════════

class SlowHandler : public RequestHandler {
public:
    // Handle() 是纯虚，异步 handler 也需提供同步兜底。
    // 返回 405：真实请求走 HandleAsync 路径，不会落到这里。
    Response Handle(const Context& ctx) override {
        return Response::Error(405, *ctx.Pool());
    }
    bool IsAsync() const override { return true; }
    // GET /slow?ms=200 → 挂起 200ms 后返回（期间不阻塞事件循环）
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");

        // 解析 ?ms= 参数（ctx.Query 由引擎自动解析）
        auto ms_str = ctx.Query("ms");
        int ms = 200;
        if (!ms_str.empty()) ms = std::atoi(std::string(ms_str).c_str());
        if (ms < 0 || ms > 5000) ms = 200;

        // co_await 挂起当前协程，事件循环去服务其他请求
        co_await coro::sleep_for(ms);

        std::string body = R"({"slept":)" + std::to_string(ms) + "}";
        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write({body.data(), body.size()});
        co_return resp;
    }
};

// ═══════════════════════════════════════════════════════════════════
// 4. 路径参数 :param —— 注册 /users/:id。
//
//    引擎按 /users/:id 模式路由，并自动捕获 "42" 注入 ctx。
//    handler 直接 ctx.Param("id") 获取，无需自解析 Path。
// ═══════════════════════════════════════════════════════════════════

class UserHandler : public RequestHandler {
public:
    // GET /users/42 → {"id":"42"}；缺参数（GET /users/）→ 400
    Response Handle(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(500, "no pool");

        std::string_view id = ctx.Param("id");
        if (id.empty()) return Response::Error(400, *pool);

        std::string body = R"({"id":")" + std::string(id) + R"("})";
        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write({body.data(), body.size()});
        return resp;
    }
};

// ═══════════════════════════════════════════════════════════════════
// 5. 通配 * —— 匹配 /files/ 下任意深度路径。
//    引擎捕获 * 之后剩余路径为参数 "path"，ctx.Param("path") 直接取。
// ═══════════════════════════════════════════════════════════════════

class FileInfoHandler : public RequestHandler {
public:
    // GET /files/a/b.txt → {"path":"a/b.txt","len":N}
    Response Handle(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(500, "no pool");

        std::string_view rel = ctx.Param("path");    // "a/b.txt"（catch-all 名）
        if (rel.empty()) rel = "?";

        std::string body = R"({"path":")" + std::string(rel) + R"("})";
        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write({body.data(), body.size()});
        return resp;
    }
};

// ═══════════════════════════════════════════════════════════════════
// 6. SSE 流式 handler —— IsStream=true，分块推送。
// ═══════════════════════════════════════════════════════════════════

class SseHandler : public RequestHandler {
public:
    // 同步路径兜底（实际请求走 HandleStream）
    Response Handle(const Context& ctx) override {
        return Response::Error(404, *ctx.Pool());
    }
    bool IsStream() const override { return true; }
    coro::Task<void> HandleStream(const Context& /*ctx*/,
                                  StreamSink& sink) override {
        for (int i = 0; i < 5 && !sink.IsDisconnected(); ++i) {
            if (!(co_await sink.PushSSE("tick=" + std::to_string(i)))) break;
            co_await coro::sleep_for(200);
        }
        sink.End();
        co_return;
    }
};

// ═══════════════════════════════════════════════════════════════════
// main —— 组装 Router + Middleware + MultiServer 并注册路由
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv)
{
    std::string config_path = "config.yaml";
    if (argc >= 3 && std::strcmp(argv[1], "-c") == 0)
        config_path = argv[2];

    auto cfg = Config::Load(config_path);
    Logger::Init(cfg.log_dir, cfg.log_level);

    // ── Router：注册路由 ──
    Router router;

    // 兜底：/ 静态文件 + 未匹配路径的 404
    auto file_cache = std::make_unique<FileCache>();
    file_cache->LoadDirectory(cfg.doc_root);
    router.Add("/", std::make_unique<StaticFileHandler>(file_cache.get()));

    // 方法限定路由（不同 method 可指向不同 handler）
    router.Get("/hello", std::make_unique<HelloHandler>());      // GET /hello
    router.Post("/echo", std::make_unique<EchoBodyHandler>());   // POST /echo
    router.Get("/slow", std::make_unique<SlowHandler>());        // GET /slow

    // Lambda 函数式注册（新接口）：无需写 handler 类，
    // 同步 handler 直接传 lambda。ctx.Query()/ctx.Param() 同样可用。
    router.Get("/greet", [](const Context& ctx) -> Response {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(500, "no pool");
        std::string_view name = ctx.Query("name");
        if (name.empty()) name = "world";
        std::string body = R"({"greet":")" + std::string(name) + R"("})";
        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write({body.data(), body.size()});
        return resp;
    });

    // :param 与 * 通配（按"最精确静态 > :param > *"优先级匹配）
    router.Get("/users/:id", std::make_unique<UserHandler>());   // GET /users/42
    router.Get("/files/*", std::make_unique<FileInfoHandler>()); // GET /files/a/b.txt

    // SSE
    router.Add("/sse", std::make_unique<SseHandler>());

    // WebSocket upgrade 路由（内部 echo 实现见 ws_echo.hpp）
    router.Add("/ws", std::make_unique<WsEchoHandler>());

    // ── 中间件（可选；与 demo 一致，也可不注册直接跑）──
    MiddlewareManager middleware;
    middleware.Add(std::make_unique<CORSMiddleware>());
    middleware.Add(std::make_unique<RequestIdMiddleware>());
    middleware.Add(std::make_unique<LoggingMiddleware>());

    // ── TLS（不想要 TLS 就把 tls_port 设 0，并跳过 Load）──
    auto tls = std::make_shared<net::TlsContext>();
    if (!tls->Load(cfg.tls_cert, cfg.tls_key)) {
        Logger::Log(LogLevel::Error, "ROUTES_DEMO",
            "TLS 证书加载失败: " + cfg.tls_cert);
        Logger::StopAll();
        return 1;
    }

    // ── 启动（与 demo_server 完全相同的接线）──
    // 注意：tls_port > 0 时单进程只监听 TLS 端口；纯明文把 tls_port 设 0。
    std::shared_ptr<MetricsCollector> metrics =
        std::make_shared<MetricsCollector>(cfg.threads);
    MultiServer server(cfg, router, middleware, tls, metrics);
    server.Start();

    Logger::StopAll();
    return 0;
}
