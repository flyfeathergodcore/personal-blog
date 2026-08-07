#pragma once
#include <string>

// 前向声明
class MetricsCollector;

// SSE 推送初始载荷
std::string SseInitialPayload(MetricsCollector* metrics);

// SSE 推送状态
struct SsePushState {
    // 初始化推送状态：保存指标收集器引用
    // 参数：metrics - 指标收集器
    void Init(MetricsCollector* metrics);
    // 构造一次 SSE 推送载荷（含时间戳与 worker 数）
    // 参数：metrics - 指标收集器
    std::string BuildPayload(MetricsCollector* metrics);

private:
    MetricsCollector* metrics_ = nullptr;
};
