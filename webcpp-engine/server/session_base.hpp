#pragma once
#include <memory>
#include "coro/task.h"
#include "protocol/session_region.hpp"
#include "router/router.hpp"
#include "middleware/middleware.hpp"
#include "protocol/response.hpp"

class MetricsCollector;

// ── SessionBase ──
//
// Protocol-agnostic base class for all HTTP session types.
//
// H11Session (HTTP/1.1) and H2Session (HTTP/2) inherit from this
// and implement their own Start() coroutine with protocol-specific
// read/parse/write logic.
//
class SessionBase : public std::enable_shared_from_this<SessionBase> {
public:
    // 虚析构函数：默认实现
    virtual ~SessionBase() = default;

    /// Main coroutine — each invocation processes the connection lifecycle.
    /// 纯虚函数：由各协议会话实现连接生命周期入口
    virtual coro::Task<void> Start() = 0;

    /// Per-connection memory region (from Worker's RegionPool).
    SessionRegion& Region() { return region_; }

    /// Attach metrics collector (called by MultiServer after construction).
    void SetMetrics(MetricsCollector* mc, int wid) {
        metrics_ = mc;
        worker_id_ = wid;
    }

    /// Set maximum request body size (0 = unlimited).
    void SetMaxBodySize(size_t bytes) { max_body_size_ = bytes; }

    /// Set WebSocket idle timeout in seconds (0 = no timeout).
    void SetWsIdleTimeout(unsigned int sec) { ws_idle_timeout_ = sec; }

protected:
    // 受保护构造函数：保存路由与中间件引用（子类构造时调用）
    // 参数：router - 路由表；middleware - 中间件管理器
    SessionBase(Router& router,
                MiddlewareManager& middleware)
        : router_(router)
        , middleware_(middleware)
    {}

    SessionRegion region_;
    Router& router_;
    MiddlewareManager& middleware_;
    MetricsCollector* metrics_ = nullptr;
    int worker_id_ = -1;
    size_t max_body_size_ = 0;   // 0 = unlimited
    unsigned int ws_idle_timeout_ = 0;  // 0 = no timeout
};
