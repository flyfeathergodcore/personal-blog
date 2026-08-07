#include "middleware/middleware.hpp"
#include "log/logger.hpp"
#include <cstdio>
#include "handler/metrics.hpp"
#include "protocol/session_region.hpp"
#include <cstring>
#include <ctime>
#include <iostream>

// ═══════════════════════════════════════════════════════════════
// MiddlewareManager
// ═══════════════════════════════════════════════════════════════

// 添加中间件：按类型挂入 pre_/post_ 链表，并接管其生命周期
// 参数：mw - 待添加的中间件（独占所有权）
void MiddlewareManager::Add(std::unique_ptr<Middleware> mw)
{
    auto type = mw->GetType();
    auto* ptr = mw.get();

    if (type == Middleware::Type::PreRequest ||
        type == Middleware::Type::Both)
        pre_.push_back(ptr);

    if (type == Middleware::Type::PostResponse ||
        type == Middleware::Type::Both)
        post_.push_back(ptr);

    owned_.push_back(std::move(mw));
}

// 原始字节阶段处理：遍历各中间件 OnRawData，任一返回有效响应即短路
// 参数：data - 原始请求字节；len - 字节长度
Response MiddlewareManager::ProcessRaw(const char* data, size_t len)
{
    for (auto* mw : pre_) {
        // PreRequest middlewares may also implement OnRawData.
        // We iterate all owned middlewares for raw data.
    }
    for (auto& mw : owned_) {
        if (auto resp = mw->OnRawData(data, len); !resp.IsNone())
            return resp;
    }
    return Response::None();
}

// 串行运行所有 PreRequest 中间件；返回 None 表示继续执行 handler
// 参数：ctx - 请求上下文（中间件可修改）
Response MiddlewareManager::ExecutePre(Context& ctx)
{
    for (auto* mw : pre_) {
        auto resp = mw->HandlePre(ctx);
        if (!resp.IsNone())
            return resp;
    }
    return Response::None();
}

// 异步串行运行所有 PostResponse 中间件
// 参数：ctx - 请求上下文；status_code - 状态码；bytes_sent - 响应字节数；elapsed_us - 请求总耗时（微秒）；worker_id - worker ID
coro::Task<void> MiddlewareManager::ExecutePost(
    const Context& ctx, int status_code,
    size_t bytes_sent, uint64_t elapsed_us, int worker_id)
{
    for (auto* mw : post_) {
        co_await mw->HandlePost(ctx, status_code, bytes_sent, elapsed_us, worker_id);
    }
    co_return;
}

// 同步串行运行所有 PostResponse 中间件（零协程帧开销）
// 参数：ctx - 请求上下文；status_code - 状态码；bytes_sent - 响应字节数；elapsed_us - 请求总耗时（微秒）；worker_id - worker ID
void MiddlewareManager::ExecutePostSync(
    const Context& ctx, int status_code,
    size_t bytes_sent, uint64_t elapsed_us, int worker_id)
{
    for (auto* mw : post_) {
        mw->HandlePostSync(ctx, status_code, bytes_sent, elapsed_us, worker_id);
    }
}

// ═══════════════════════════════════════════════════════════════
// CORSMiddleware
// ═══════════════════════════════════════════════════════════════

// CORS 预检处理：OPTIONS 直接返回 204，其余注入跨域响应头
// 参数：ctx - 请求上下文
Response CORSMiddleware::HandlePre(Context& ctx)
{
    if (ctx.Method() == "OPTIONS") {
        return Response::Raw(204,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
    }

    // Inject CORS header — handler will read it via ctx.ResponseHeaders()
    // and write it to the response via resp.Header().
    ctx.AddResponseHeader("Access-Control-Allow-Origin", "*");
    return Response::None();
}

// ═══════════════════════════════════════════════════════════════
// RequestIdMiddleware — X-Request-Id forwarding / generation
// ═══════════════════════════════════════════════════════════════

// 生成短请求 ID（线程本地计数器 + 线程 ID），写入 pool 避免堆分配
// 参数：pool - 会话区域，用于存放 ID 字符串；返回指向 pool 内 ID 的视图
std::string_view RequestIdMiddleware::GenerateId(SessionRegion& pool)
{
    thread_local static uint64_t counter = 0;
    counter++;
    // Compact ID: worker_tid_counter (hex) — written directly into pool
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lx_%lx",
                          (unsigned long)pthread_self(),
                          (unsigned long)counter);
    auto off = pool.DupOff({buf, static_cast<size_t>(n)});
    return pool.ToView(off);
}

// 转发或生成 X-Request-Id，并注入请求上下文与响应头
// 参数：ctx - 请求上下文
Response RequestIdMiddleware::HandlePre(Context& ctx)
{
    auto existing = ctx.Header("x-request-id");
    if (!existing.empty()) {
        ctx.SetRequestId(existing);
        ctx.AddResponseHeader("X-Request-Id", existing);
    } else {
        auto* pool = ctx.Pool();
        if (pool) {
            auto id = GenerateId(*pool);
            ctx.SetRequestId(id);
            ctx.AddResponseHeader("X-Request-Id", id);
        }
    }
    return Response::None();
}

// ═══════════════════════════════════════════════════════════════
// LoggingMiddleware — structured JSON log
// ═══════════════════════════════════════════════════════════════

// 生成日志时间戳（UTC，按秒缓存），写入 pool 避免堆分配
// 参数：pool - 会话区域，用于存放时间戳字符串
static std::string_view LogTimestamp(SessionRegion& pool)
{
    static time_t last = 0;
    static char buf[32];
    static size_t len = 0;
    auto now = ::time(nullptr);
    if (now != last) {
        last = now;
        struct tm tm;
        ::gmtime_r(&now, &tm);
        len = ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    auto off = pool.DupOff({buf, len});
    return pool.ToView(off);
}

// 同步后置处理：构建结构化 JSON 访问日志并写入网关日志
// 参数：ctx - 请求上下文；status_code - 状态码；bytes_sent - 响应字节数；elapsed_us - 请求耗时（微秒）；worker_id - 预留，未使用
void LoggingMiddleware::HandlePostSync(
    const Context& ctx,
    int status_code,
    size_t bytes_sent,
    uint64_t elapsed_us,
    int /*worker_id*/)
{
    auto* pool = const_cast<Context&>(ctx).Pool();
    if (!pool)
        return;

    // Build JSON line — thread_local buffer, no per-request alloc
    thread_local std::string j;
    j.clear();
    j += R"({"t":")";
    j += LogTimestamp(*pool);          // pool-backed, no alloc
    j += R"(","m":")";
    j += ctx.Method();
    j += R"(","p":")";
    j += ctx.Path();
    j += R"(","s":)";

    // std::to_string → snprintf to stack buffer
    char num[24];
    int n;
    n = std::snprintf(num, sizeof(num), "%d", status_code);
    j.append(num, static_cast<size_t>(n));

    j += R"(,"d":)";
    n = std::snprintf(num, sizeof(num), "%lu", (unsigned long)elapsed_us);
    j.append(num, static_cast<size_t>(n));

    j += R"(,"b":)";
    n = std::snprintf(num, sizeof(num), "%zu", bytes_sent);
    j.append(num, static_cast<size_t>(n));

    j += R"(,"h2":)";
    j += ctx.IsHttp2() ? "true" : "false";
    j += R"(,"id":")";
    j += ctx.RequestId();
    j += '"';
    j += '}';

    // 使用新三层日志系统写入网关日志（直接传 string_view，避免每请求 2 次堆分配）
    Logger::Instance().Gateway(
        ctx.Method(), ctx.Path(),
        status_code, elapsed_us);
}

// 异步后置处理：委托给同步实现
// 参数：ctx - 请求上下文；status_code - 状态码；bytes_sent - 响应字节数；elapsed_us - 请求耗时（微秒）；worker_id - worker ID
coro::Task<void> LoggingMiddleware::HandlePost(
    const Context& ctx,
    int status_code,
    size_t bytes_sent,
    uint64_t elapsed_us,
    int worker_id)
{
    HandlePostSync(ctx, status_code, bytes_sent, elapsed_us, worker_id);
    co_return;
}

// ═══════════════════════════════════════════════════════════════
// MetricsMiddleware
// ═══════════════════════════════════════════════════════════════

// PreRequest：拦截 /metrics.json、/dashboard、/metrics/stream 等指标端点
// 参数：ctx - 请求上下文
Response MetricsMiddleware::HandlePre(Context& ctx)
{
    auto path = ctx.Path();

    if (path == "/metrics.json")
    {
        auto json = collector_->RenderMetricsJson();
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(200, std::move(json));

        Response resp(200, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", json.size());
        resp.EndHeaders();
        pool->Write(json);
        return resp;
    }

    // Redirect /dashboard → /dashboard/
    if (path == "/dashboard")
    {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(301,
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: /dashboard/\r\n"
            "Content-Length: 0\r\n\r\n");
        Response resp(301, *pool);
        resp.Header("Location", "/dashboard/");
        resp.EndHeaders();
        return resp;
    }

    if (path == "/metrics/stream" || path == "/metrics/stream/")
    {
        auto* pool = ctx.Pool();
        if (!pool)
            return Response::Raw(200, "data: {\"error\":\"no pool\"}\n\n");

        int interval_ms = kDefaultPushMs;
        return Response::SSEStream(*pool, interval_ms);
    }

    return Response::None();
}

// 同步后置处理：把本次请求指标上报到 MetricsCollector
// 参数：ctx - 请求上下文；status_code - 状态码；bytes_sent - 响应字节数；elapsed_us - 请求耗时（微秒）；worker_id - worker ID
void MetricsMiddleware::HandlePostSync(
    const Context& ctx,
    int status_code,
    size_t bytes_sent,
    uint64_t elapsed_us,
    int worker_id)
{
    if (collector_)
        collector_->OnRequest(elapsed_us, status_code, bytes_sent, worker_id,
                               ctx.IsHttp2());
}

// 异步后置处理：委托给同步实现
// 参数：ctx - 请求上下文；status_code - 状态码；bytes_sent - 响应字节数；elapsed_us - 请求耗时（微秒）；worker_id - worker ID
coro::Task<void> MetricsMiddleware::HandlePost(
    const Context& ctx,
    int status_code,
    size_t bytes_sent,
    uint64_t elapsed_us,
    int worker_id)
{
    HandlePostSync(ctx, status_code, bytes_sent, elapsed_us, worker_id);
    co_return;
}
