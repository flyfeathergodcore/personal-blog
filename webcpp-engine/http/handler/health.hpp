#pragma once
#include "http/handler/request_handler.hpp"

// ═══════════════════════════════════════════════════════════════════
// HealthHandler — returns {"status":"ok"} for health checks
//
// Used by load balancers and orchestrators (k8s liveness/readiness
// probes).  Always returns 200 with a minimal JSON body.
// ═══════════════════════════════════════════════════════════════════

class HealthHandler : public RequestHandler {
public:
    // 健康检查处理：始终返回 200 + {"status":"ok"}
    // 参数：ctx - 请求上下文
    Response Handle(const Context& ctx) override;
};
