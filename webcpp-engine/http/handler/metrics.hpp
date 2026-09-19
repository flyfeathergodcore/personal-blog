#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include "http/config/config.hpp"

// ═══════════════════════════════════════════════════════════════
// Metrics — thread-local counters, lock-free ring buffer
// ═══════════════════════════════════════════════════════════════

constexpr int kLatencyBuckets  = 10;
constexpr int kRingHistory     = 60;   // 60-second sliding window
constexpr int kMaxWorkers      = 64;
constexpr int kDefaultPushMs   = 1000; // default SSE push interval

// ── 运行时指标参数（热更新全局，读这里不读启动时 Load 一次的 Config）──
//
// 默认值来自 config.yaml 的 metrics: 段；后台「站点设置 → 指标与统计」
// 在线修改后写 site_config(key='metrics_config') 并热应用到本结构体，
// 重启容器回落 yaml 默认值。消费方：FlushLoop（flush/persist）、
// persist 回调（cleanup）、前端 Dashboard（realtime/trend/visitor）。
// 字段全部用 std::atomic<int>，保证多线程读安全。
// 注意：与 config.hpp MetricsConfig、前端 defaultMetricsConfig 保持一致。
struct RuntimeMetricsConfig {
    std::atomic<int> flush_interval_ms      {1000};  // QPS 刷新周期（Flush 间隔，毫秒，100–1000）
    std::atomic<int> persist_interval_secs  {60};    // 统计落库周期（秒，10–3600）
    std::atomic<int> cleanup_interval_secs  {3600};  // 过期统计清理周期（秒，300–86400）
    std::atomic<int> cleanup_retention_days {30};    // 清理保留窗口（天，7–3650）
    std::atomic<int> realtime_refresh_ms    {3000};  // 前端仪表盘实时轮询（毫秒，1000–60000）
    std::atomic<int> trend_refresh_ms       {60000}; // 前端仪表盘趋势图轮询（毫秒，5000–3600000）
    std::atomic<int> visitor_refresh_ms     {5000};  // 前端仪表盘访问者表轮询（毫秒，1000–60000）
};
// 全局运行时配置（定义在 metrics.cpp）。注意：RuntimeMetricsConfig 的默认
// 值仅是兜底；真实默认由 ApplyMetricsConfig(cfg.metrics) 从 yaml 装载。
extern RuntimeMetricsConfig g_metrics_cfg;

// 将 yaml 装载的 MetricsConfig 应用到运行时全局（带 clamp 区间校验）
void ApplyMetricsConfig(const MetricsConfig& cfg);
// 序列化当前运行时配置为 JSON（GET /api/metrics-config 响应 / POST 回显 / 落库共用）
std::string SerializeMetricsConfigJson();

/// Bucket upper bounds in microseconds.
constexpr uint64_t kBucketMax[kLatencyBuckets] = {
    64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000, UINT64_MAX
};

// ── Per-worker hot counters (non-atomic, accessed by one thread only) ──
//
struct alignas(64) WorkerMetrics {
    /// Active connections — atomic because it's read by the HTTP handler
    /// while being written by the session loop.
    std::atomic<uint64_t> active_connections{0};

    uint64_t request_count  = 0;   // combined (H1+H2)
    uint64_t request_h1     = 0;
    uint64_t request_h2     = 0;
    uint64_t error_count    = 0;
    uint64_t error_h1       = 0;
    uint64_t error_h2       = 0;
    uint64_t bytes_sent     = 0;
    uint64_t latency_buckets[kLatencyBuckets] = {};
};

// ── Per-second snapshot from one worker ──
//
struct MetricsSnapshot {
    uint64_t request_count  = 0;   // combined (H1+H2)
    uint64_t request_h1     = 0;
    uint64_t request_h2     = 0;
    uint64_t error_count    = 0;
    uint64_t error_h1       = 0;
    uint64_t error_h2       = 0;
    uint64_t bytes_sent     = 0;
    uint64_t latency_buckets[kLatencyBuckets] = {};

    // 新增 RPC 指标
    struct RpcMetrics {
        std::map<std::string, uint64_t> rpc_requests;      // 各服务请求数
        std::map<std::string, uint64_t> rpc_errors;        // 各服务错误数
        std::map<std::string, uint64_t> rpc_retries;       // 各服务重试次数
        std::map<std::string, int> instance_count;         // 活跃实例数
        std::map<std::string, int> healthy_instances;      // 健康实例数
        std::map<std::string, uint64_t> rpc_latency_us;    // 平均延迟
    } rpc;
};

// ── One ring slot: all workers' snapshots for one second ──
//
struct RingSlot {
    int64_t           timestamp = 0;
    MetricsSnapshot   workers[kMaxWorkers];
    MetricsSnapshot   total;
    // 本秒活动连接采样（Flush 时仅 worker0 填写；落库聚合时求平均/峰值）
    uint64_t          active_conn_sample = 0;
};

// ── Percentile computation ──
//
struct LatencyPercentiles {
    uint64_t p50 = 0, p90 = 0, p99 = 0;
};
// 根据直方图桶计数计算 p50/p90/p99 延迟分位数
// 参数：buckets - 各桶计数的延迟直方图数组
LatencyPercentiles ComputePercentiles(const uint64_t buckets[kLatencyBuckets]);

// 60s 聚合窗口（落库 site_stats 用）
struct StatsWindow {
    int64_t ts = 0;                 // 窗口结束时刻（unix 秒）
    uint64_t req = 0, req_h1 = 0, req_h2 = 0;
    uint64_t err = 0;
    uint64_t bytes = 0;
    LatencyPercentiles per;         // 全窗口 latency_buckets 合并后重算的分位
    double act_avg = 0;             // 活动连接平均值
    uint64_t act_max = 0;           // 活动连接峰值
};

// ═══════════════════════════════════════════════════════════════
// Alert system
// ═══════════════════════════════════════════════════════════════

enum class AlertMetric {
    ErrorRate,    // percentage (0.0 – 100.0)
    P99Latency,   // microseconds
    QPS,          // requests/second
};

struct AlertRule {
    std::string   name;
    AlertMetric   metric;
    double        threshold;   // e.g. 0.1 for 0.1% error rate, 10000 for 10ms p99
    int           window_secs; // evaluate over last N seconds
};

/// Current state of one alert.
struct AlertState {
    std::string name;
    bool        firing = false;

    /// JSON snippet: {"name":"...","state":"firing|ok","value":...,"threshold":...}
    std::string ToJson(double current_value) const;
};

// ═══════════════════════════════════════════════════════════════
// MetricsCollector
// ═══════════════════════════════════════════════════════════════

class MetricsCollector {
public:
    // 构造指标收集器
    // 参数：num_workers - 初始 worker 线程数
    explicit MetricsCollector(int num_workers);

    // 更新 worker 线程数
    // 参数：n - worker 线程数
    void SetWorkerCount(int n) { num_workers_ = n; }
    // 返回当前 worker 线程数
    int  WorkerCount() const { return num_workers_; }

    // Alerts
    // 设置告警规则
    // 参数：rules - 告警规则列表
    void SetAlertRules(std::vector<AlertRule> rules) { alert_rules_ = std::move(rules); }
    // 返回当前告警状态列表（只读）
    const std::vector<AlertState>& AlertStates() const { return alert_states_; }

    // ── Hot path (from Session) ──

    // 记录一次请求（每 worker 热路径）
    // 参数：latency_us - 请求耗时（微秒）；status_code - 状态码；bytes - 响应字节数；wid - worker ID；is_h2 - 是否 HTTP/2
    void OnRequest(uint64_t latency_us, int status_code,
                   size_t bytes, int wid, bool is_h2);
    // 记录连接建立
    // 参数：wid - worker ID
    void OnConnectionOpen(int wid);
    // 记录连接关闭
    // 参数：wid - worker ID
    void OnConnectionClose(int wid);

    // ── Per-worker flush (1-second timer) ──

    // 将指定 worker 的计数器快照写入环形缓冲区
    // 参数：wid - worker ID
    void Flush(int wid);

    // ── HTTP response builders ──

    // 渲染全量指标 JSON（/metrics.json 用）
    std::string RenderMetricsJson() const;

    // ── Accessors ──

    // 返回当前活动连接数（全 worker 求和）
    uint64_t ActiveConnections() const;
    // 返回当前时间戳（unix 秒）
    int64_t  CurrentTimestamp() const;

    /// 聚合最近 ≤60s 有数据的 slot 到 StatsWindow；窗口全 0（无访问）返回 false
    bool SumLast60s(StatsWindow& out) const;

private:
    int num_workers_;
    std::array<WorkerMetrics, kMaxWorkers> workers_{};
    std::array<RingSlot, kRingHistory> ring_{};
    // 每个 worker 最近一次刷屏的 unix 秒（同秒累加判定：flush 周期 <1s 时
    // 同秒多次刷屏需累加进既有槽而非覆盖，避免丢数据）。各元素仅由对应
    // worker 线程写入（Flush 固定线程），无跨线程竞态，无需原子。
    int64_t last_flush_secs_[kMaxWorkers] = {};
    std::chrono::steady_clock::time_point start_;

    // Alerts
    std::vector<AlertRule>  alert_rules_;
    std::vector<AlertState> alert_states_;

    // 根据当前时间戳评估各告警规则，更新告警状态
    // 参数：now_ts - 当前时间戳（unix 秒）
    void EvaluateAlerts(int64_t now_ts);
};
