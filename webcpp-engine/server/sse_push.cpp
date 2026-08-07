#include "server/sse_push.hpp"
#include "handler/metrics.hpp"
#include <sstream>
#include <chrono>

// 构造 SSE 初始连接通知载荷（JSON 文本）；metrics 为空时返回错误载荷
// 参数：metrics - 指标收集器（可空）
std::string SseInitialPayload(MetricsCollector* metrics) {
    if (!metrics) {
        return "data: {\"error\":\"no metrics\"}\n\n";
    }

    std::ostringstream oss;
    oss << "data: {\"status\":\"connected\",\"workers\":"
        << metrics->WorkerCount() << "}\n\n";
    return oss.str();
}

// 初始化推送状态：保存指标收集器引用
// 参数：metrics - 指标收集器
void SsePushState::Init(MetricsCollector* metrics) {
    metrics_ = metrics;
}

// 构造一次 SSE 推送载荷（含时间戳与 worker 数）；metrics 为空时返回错误载荷
// 参数：metrics - 指标收集器（可空）
std::string SsePushState::BuildPayload(MetricsCollector* metrics) {
    if (!metrics) {
        return "data: {\"error\":\"no metrics\"}\n\n";
    }

    // 简化实现：返回基本指标
    std::ostringstream oss;
    oss << "data: {\"timestamp\":" << std::chrono::system_clock::now().time_since_epoch().count()
        << ",\"workers\":" << metrics->WorkerCount()
        << "}\n\n";
    return oss.str();
}
