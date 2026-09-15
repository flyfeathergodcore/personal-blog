#include "handler/metrics.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cassert>

// ═══════════════════════════════════════════════════════════════
// 运行时指标参数（热更新全局，读这里不读启动时 Load 一次的 Config）
// ═══════════════════════════════════════════════════════════════

namespace {
// 数值钳制到 [lo, hi] 区间（防止配置文件 / 在线接口传值越界）
int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
} // namespace

RuntimeMetricsConfig g_metrics_cfg;

// 应用指标参数配置到运行时全局（yaml 装载与后台在线修改共用），逐字段 clamp
// 参数：cfg - 来源配置（yaml 默认值或在线表单值）
void ApplyMetricsConfig(const MetricsConfig& cfg)
{
    g_metrics_cfg.flush_interval_ms.store(ClampInt(cfg.flush_interval_ms, 100, 1000));
    g_metrics_cfg.persist_interval_secs.store(ClampInt(cfg.persist_interval_secs, 10, 3600));
    g_metrics_cfg.cleanup_interval_secs.store(ClampInt(cfg.cleanup_interval_secs, 300, 86400));
    g_metrics_cfg.cleanup_retention_days.store(ClampInt(cfg.cleanup_retention_days, 7, 3650));
    g_metrics_cfg.realtime_refresh_ms.store(ClampInt(cfg.realtime_refresh_ms, 1000, 60000));
    g_metrics_cfg.trend_refresh_ms.store(ClampInt(cfg.trend_refresh_ms, 5000, 3600000));
    g_metrics_cfg.visitor_refresh_ms.store(ClampInt(cfg.visitor_refresh_ms, 1000, 60000));
}

// 序列化当前运行时指标参数为 JSON（GET /api/metrics-config 响应、
// POST 回显、落库 site_config(key='metrics_config') 共用）
std::string SerializeMetricsConfigJson()
{
    std::string j;
    j.reserve(256);
    j += "{\"flush_interval_ms\":";
    j += std::to_string(g_metrics_cfg.flush_interval_ms.load());
    j += ",\"persist_interval_secs\":";
    j += std::to_string(g_metrics_cfg.persist_interval_secs.load());
    j += ",\"cleanup_interval_secs\":";
    j += std::to_string(g_metrics_cfg.cleanup_interval_secs.load());
    j += ",\"cleanup_retention_days\":";
    j += std::to_string(g_metrics_cfg.cleanup_retention_days.load());
    j += ",\"realtime_refresh_ms\":";
    j += std::to_string(g_metrics_cfg.realtime_refresh_ms.load());
    j += ",\"trend_refresh_ms\":";
    j += std::to_string(g_metrics_cfg.trend_refresh_ms.load());
    j += ",\"visitor_refresh_ms\":";
    j += std::to_string(g_metrics_cfg.visitor_refresh_ms.load());
    j += "}";
    return j;
}

// ═══════════════════════════════════════════════════════════════
// Percentile computation
// ═══════════════════════════════════════════════════════════════

// 由延迟直方图桶计算 p50/p90/p99 分位数（桶内线性插值，末桶取固定扩展）
// 参数：buckets - 各延迟桶的计数数组
LatencyPercentiles ComputePercentiles(const uint64_t buckets[kLatencyBuckets])
{
    LatencyPercentiles p{};

    constexpr uint64_t lower_bound[kLatencyBuckets] = {
        0, 64, 128, 256, 512, 1000, 2000, 4000, 8000, 16000
    };

    uint64_t total = 0;
    for (int i = 0; i < kLatencyBuckets; i++)
        total += buckets[i];

    if (total == 0) return p;

    constexpr uint64_t targets[3] = {50, 90, 99};
    uint64_t* results[3] = {&p.p50, &p.p90, &p.p99};
    int ti = 0;

    uint64_t cum = 0;
    for (int i = 0; i < kLatencyBuckets && ti < 3; i++)
    {
        if (buckets[i] == 0) continue;
        uint64_t prev_cum = cum;
        cum += buckets[i];
        double threshold = static_cast<double>(targets[ti]) / 100.0 * total;

        while (ti < 3 && static_cast<double>(cum) >= threshold)
        {
            double frac = 0.0;
            if (cum != prev_cum)
                frac = (threshold - prev_cum) / (cum - prev_cum);
            // For the last bucket (≥16ms), UINT64_MAX as upper bound causes
            // garbage in double arithmetic. Use a fixed 16ms extension.
            uint64_t range = (i == kLatencyBuckets - 1)
                ? kBucketMax[kLatencyBuckets - 2]  // 16000
                : (kBucketMax[i] - lower_bound[i]);
            *results[ti] = lower_bound[i]
                + static_cast<uint64_t>(frac * static_cast<double>(range));
            ti++;
            if (ti < 3)
                threshold = static_cast<double>(targets[ti]) / 100.0 * total;
        }
    }

    for (; ti < 3; ti++)
        *results[ti] = 16000;

    return p;
}

// ═══════════════════════════════════════════════════════════════
// AlertState
// ═══════════════════════════════════════════════════════════════

// 将告警状态序列化为 JSON 字符串
// 参数：current_value - 当前指标值（写入 JSON 的 value 字段）
std::string AlertState::ToJson(double current_value) const
{
    std::string j = "{\"name\":\"";
    j += name;
    j += "\",\"state\":\"";
    j += firing ? "firing" : "ok";
    j += "\",\"value\":";
    j += std::to_string(current_value);
    j += "}";
    return j;
}

// ═══════════════════════════════════════════════════════════════
// MetricsCollector
// ═══════════════════════════════════════════════════════════════

// 构造：记录启动时间并初始化默认告警规则（错误率/P99/QPS 骤降）
// 参数：num_workers - worker 数量
MetricsCollector::MetricsCollector(int num_workers)
    : num_workers_(num_workers)
    , start_(std::chrono::steady_clock::now())
{
    // Default alert rules
    alert_rules_ = {
        {"high-error-rate", AlertMetric::ErrorRate, 0.1, 5},   // >0.1% errors
        {"high-p99",       AlertMetric::P99Latency, 10000, 5},  // >10ms
        {"qps-plummet",    AlertMetric::QPS,        1000,   10}, // <1000 qps
    };
    alert_states_.resize(alert_rules_.size());
    for (size_t i = 0; i < alert_rules_.size(); i++)
        alert_states_[i].name = alert_rules_[i].name;
}

// 记录一次请求：累加请求/错误计数、发送字节，并按延迟归入分位桶
// 参数：latency_us - 延迟（微秒）；status_code - 响应状态码；bytes - 发送字节数；wid - worker 编号；is_h2 - 是否 HTTP/2
void MetricsCollector::OnRequest(uint64_t latency_us, int status_code,
                                  size_t bytes, int wid, bool is_h2)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];
    w.request_count++;
    if (is_h2) w.request_h2++;
    else       w.request_h1++;

    if (status_code < 200 || status_code >= 300) {
        w.error_count++;
        if (is_h2) w.error_h2++;
        else       w.error_h1++;
    }

    w.bytes_sent += bytes;

    int bucket = kLatencyBuckets - 1;
    for (int i = 0; i < kLatencyBuckets - 1; i++)
    {
        if (latency_us < kBucketMax[i]) {
            bucket = i;
            break;
        }
    }
    w.latency_buckets[bucket]++;
}

// 记录连接建立：对应 worker 的活动连接数 +1
// 参数：wid - worker 编号
void MetricsCollector::OnConnectionOpen(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    workers_[wid].active_connections.fetch_add(1, std::memory_order_relaxed);
}

// 记录连接关闭：对应 worker 的活动连接数 -1
// 参数：wid - worker 编号
void MetricsCollector::OnConnectionClose(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    workers_[wid].active_connections.fetch_sub(1, std::memory_order_relaxed);
}

// 周期刷屏：拷贝并清零该 worker 的计数快照写入环形缓冲（worker 0 额外采样活动连接与评估告警）
// 参数：wid - worker 编号
void MetricsCollector::Flush(int wid)
{
    if (wid < 0 || wid >= kMaxWorkers) return;
    auto& w = workers_[wid];

    MetricsSnapshot snap;
    snap.request_count = w.request_count;
    snap.request_h1    = w.request_h1;
    snap.request_h2    = w.request_h2;
    snap.error_count   = w.error_count;
    snap.error_h1      = w.error_h1;
    snap.error_h2      = w.error_h2;
    snap.bytes_sent    = w.bytes_sent;
    std::memcpy(snap.latency_buckets, w.latency_buckets,
                sizeof(snap.latency_buckets));

    w.request_count = 0;
    w.request_h1    = 0;
    w.request_h2    = 0;
    w.error_count   = 0;
    w.error_h1      = 0;
    w.error_h2      = 0;
    w.bytes_sent    = 0;
    std::memset(w.latency_buckets, 0, sizeof(w.latency_buckets));

    auto now = std::chrono::steady_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    int slot = static_cast<int>(secs % kRingHistory);

    // 同秒累加：flush 周期 <1s 时，本 worker 同秒内多次刷屏，把新快照累加
    // 进既有槽（不覆盖、不丢数据）；跨秒才覆盖写新槽并推进本秒标记。
    // 用 last_flush_secs_[wid]（本 worker 上次刷屏秒）判定而非共享槽
    // timestamp——后者可能被其它 worker 写，无法区分"本 worker 已刷过本秒"。
    bool new_second = (last_flush_secs_[wid] != secs);
    if (new_second) {
        last_flush_secs_[wid] = secs;
        ring_[slot].timestamp = secs;
        ring_[slot].workers[wid] = snap;
    } else {
        auto& ws = ring_[slot].workers[wid];
        ws.request_count += snap.request_count;
        ws.request_h1    += snap.request_h1;
        ws.request_h2    += snap.request_h2;
        ws.error_count   += snap.error_count;
        ws.error_h1      += snap.error_h1;
        ws.error_h2      += snap.error_h2;
        ws.bytes_sent    += snap.bytes_sent;
        for (int b = 0; b < kLatencyBuckets; b++)
            ws.latency_buckets[b] += snap.latency_buckets[b];
        // RPC 图：当前未填充，防御性累加以便未来扩展（instance/healthy 是
        // 「当前值」语义，保持覆盖不累加）
        for (auto& kv : snap.rpc.rpc_requests)   ws.rpc.rpc_requests[kv.first]  += kv.second;
        for (auto& kv : snap.rpc.rpc_errors)     ws.rpc.rpc_errors[kv.first]    += kv.second;
        for (auto& kv : snap.rpc.rpc_retries)    ws.rpc.rpc_retries[kv.first]   += kv.second;
        for (auto& kv : snap.rpc.rpc_latency_us) ws.rpc.rpc_latency_us[kv.first] += kv.second;
    }

    MetricsSnapshot total{};
    for (int i = 0; i < kMaxWorkers; i++)
    {
        auto& ws = ring_[slot].workers[i];
        total.request_count += ws.request_count;
        total.request_h1    += ws.request_h1;
        total.request_h2    += ws.request_h2;
        total.error_count   += ws.error_count;
        total.error_h1      += ws.error_h1;
        total.error_h2      += ws.error_h2;
        total.bytes_sent    += ws.bytes_sent;
        for (int b = 0; b < kLatencyBuckets; b++)
            total.latency_buckets[b] += ws.latency_buckets[b];
    }
    ring_[slot].total = total;

    // 采样活动连接：仅 worker 0 且跨秒时写（同秒多次刷屏只采一次，
    // 保证每秒一采样，行为与默认 1s flush 一致）
    if (wid == 0 && new_second) ring_[slot].active_conn_sample = ActiveConnections();

    // Evaluate alerts on worker 0's flush only (avoid duplicate evaluation)
    if (wid == 0)
        EvaluateAlerts(secs);
}

// 依据环形缓冲内窗口聚合数据评估各告警规则，更新告警触发状态
// 参数：now_ts - 当前 Unix 秒时间戳
void MetricsCollector::EvaluateAlerts(int64_t now_ts)
{
    // Gather per-second totals over the window from the ring buffer
    for (size_t i = 0; i < alert_rules_.size(); i++)
    {
        auto& rule = alert_rules_[i];
        auto& state = alert_states_[i];

        // Sum over the window
        uint64_t sum_requests = 0;
        uint64_t sum_errors   = 0;
        uint64_t sum_p99_buckets[kLatencyBuckets] = {};

        for (int s = 0; s < std::min(rule.window_secs, kRingHistory); s++)
        {
            int idx = static_cast<int>((now_ts - s) % kRingHistory);
            auto& slot = ring_[idx];
            if (slot.timestamp == 0) continue;
            // Only include slots within window_secs of now
            if (now_ts - slot.timestamp > rule.window_secs) continue;

            sum_requests += slot.total.request_count;
            sum_errors   += slot.total.error_count;
            for (int b = 0; b < kLatencyBuckets; b++)
                sum_p99_buckets[b] += slot.total.latency_buckets[b];
        }

        double current_value = 0.0;
        bool breached = false;

        switch (rule.metric)
        {
        case AlertMetric::ErrorRate:
            current_value = (sum_requests > 0)
                ? 100.0 * static_cast<double>(sum_errors) / sum_requests
                : 0.0;
            breached = (current_value > rule.threshold);
            break;

        case AlertMetric::P99Latency: {
            auto per = ComputePercentiles(sum_p99_buckets);
            current_value = static_cast<double>(per.p99);
            breached = (current_value > rule.threshold);
            break;
        }

        case AlertMetric::QPS: {
            double avg_qps = (rule.window_secs > 0)
                ? static_cast<double>(sum_requests) / rule.window_secs
                : 0.0;
            current_value = avg_qps;
            // threshold is the MINIMUM, breach when below
            breached = (rule.threshold > 0 && current_value < rule.threshold);
            break;
        }
        }

        state.firing = breached;
        // current_value is stored for the delta renderer
        // but we don't keep it per-alert; it's computed on-demand.
        (void)current_value;
    }
}

// 汇总全部 worker 当前活动连接数
uint64_t MetricsCollector::ActiveConnections() const
{
    uint64_t total = 0;
    for (int i = 0; i < kMaxWorkers; i++)
        total += workers_[i].active_connections.load(std::memory_order_relaxed);
    return total;
}

// 返回 steady_clock 的 Unix 秒时间戳（用于环形缓冲 slot 定位）
int64_t MetricsCollector::CurrentTimestamp() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ═══════════════════════════════════════════════════════════════
// JSON rendering (full history)
// ═══════════════════════════════════════════════════════════════

// 向 JSON 追加一条历史数据点（含 h1/h2 拆分与分位数），非首条前加逗号换行
// 参数：json - 目标字符串；ts - 时间戳；qps/err/bytes - 请求/错误/字节数；per - 分位数；
//       act - 活动连接；first - 是否首条；qps_h1/qps_h2/err_h1/err_h2 - 协议拆分计数
static void AppendJsonEntry(std::string& json, int64_t ts,
                             uint64_t qps, uint64_t err, uint64_t bytes,
                             const LatencyPercentiles& per,
                             uint64_t act,
                             bool first,
                             uint64_t qps_h1 = 0, uint64_t qps_h2 = 0,
                             uint64_t err_h1 = 0, uint64_t err_h2 = 0)
{
    if (!first) json += ",\n";
    json += "    {\"t\":";
    json += std::to_string(ts);
    json += ",\"qps\":";
    json += std::to_string(qps);
    json += ",\"qps_h1\":";
    json += std::to_string(qps_h1);
    json += ",\"qps_h2\":";
    json += std::to_string(qps_h2);
    json += ",\"err\":";
    json += std::to_string(err);
    json += ",\"err_h1\":";
    json += std::to_string(err_h1);
    json += ",\"err_h2\":";
    json += std::to_string(err_h2);
    json += ",\"bytes\":";
    json += std::to_string(bytes);
    json += ",\"p50\":";
    json += std::to_string(per.p50);
    json += ",\"p90\":";
    json += std::to_string(per.p90);
    json += ",\"p99\":";
    json += std::to_string(per.p99);
    json += ",\"act\":";
    json += std::to_string(act);
    json += "}";
}

// 渲染完整指标 JSON：活动连接、运行时长、告警状态与环形缓冲历史
std::string MetricsCollector::RenderMetricsJson() const
{
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        now - start_).count();

    auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    int current_slot = static_cast<int>(now_secs % kRingHistory);

    std::string json;
    json.reserve(8192);

    json += "{\n";
    json += "  \"active_connections\": ";
    json += std::to_string(ActiveConnections());
    json += ",\n";
    json += "  \"uptime_seconds\": ";
    json += std::to_string(uptime);
    json += ",\n";
    json += "  \"alerts\": [\n";

    // Alert states
    for (size_t i = 0; i < alert_states_.size(); i++)
    {
        if (i > 0) json += ",\n";
        json += "    ";
        json += alert_states_[i].ToJson(0.0);
    }
    json += "\n  ],\n";

    json += "  \"history\": [\n";

    int count = 0;
    for (int i = 0; i < kRingHistory; i++)
    {
        int idx = (current_slot + 1 + i) % kRingHistory;
        auto& slot = ring_[idx];
        if (slot.timestamp == 0) continue;
        if (slot.total.request_count == 0 && slot.total.error_count == 0) continue;

        auto per = ComputePercentiles(slot.total.latency_buckets);
        AppendJsonEntry(json, slot.timestamp,
                         slot.total.request_count,
                         slot.total.error_count,
                         slot.total.bytes_sent,
                         per, ActiveConnections(),
                         count == 0,
                         slot.total.request_h1,
                         slot.total.request_h2,
                         slot.total.error_h1,
                         slot.total.error_h2);
        count++;
    }

    json += "\n  ]\n}\n";
    return json;
}

// 汇总最近 60s 指标窗口供落库（site_stats）：该分钟无访问则返回 false 跳过
// 参数：out - 汇总输出（含真实系统时钟时间戳）；返回是否有有效数据
bool MetricsCollector::SumLast60s(StatsWindow& out) const
{
    out = StatsWindow{};
    uint64_t buckets[kLatencyBuckets] = {};
    uint64_t act_sum = 0, act_max = 0, act_count = 0;
    bool any = false;

    const int64_t now_secs = CurrentTimestamp();
    const int cur = static_cast<int>(now_secs % kRingHistory);
    // 从当前 slot 向前回溯 60 个，取有数据且未过期的秒
    for (int i = 0; i < kRingHistory; i++) {
        int idx = (cur - i + kRingHistory) % kRingHistory;
        auto& slot = ring_[idx];
        if (slot.timestamp == 0) continue;
        if (now_secs - slot.timestamp > 60) continue;   // 只看最近 60s
        any = true;
        out.req    += slot.total.request_count;
        out.req_h1 += slot.total.request_h1;
        out.req_h2 += slot.total.request_h2;
        out.err    += slot.total.error_count;
        out.bytes  += slot.total.bytes_sent;
        for (int b = 0; b < kLatencyBuckets; b++)
            buckets[b] += slot.total.latency_buckets[b];
        if (slot.active_conn_sample > 0) {
            act_sum += slot.active_conn_sample;
            act_count++;
            if (slot.active_conn_sample > act_max) act_max = slot.active_conn_sample;
        }
    }
    if (!any) return false;
    // ts 落库需要真实日期时间：用 system_clock（unix 秒）。
    // 注意不能复用 now_secs（steady_clock，自系统启动计时），
    // 否则 localtime_r 转出的年份会回到 1970。
    out.ts     = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    out.per    = ComputePercentiles(buckets);
    out.act_avg = act_count ? static_cast<double>(act_sum) / act_count : 0.0;
    out.act_max = act_max;
    // 该分钟确实无任何访问则跳过落库（避免凌晨刷空行）
    return out.req != 0 || out.err != 0;
}

// ═══════════════════════════════════════════════════════════════
// Dashboard HTML (embedded, uses EventSource for SSE)
// ═══════════════════════════════════════════════════════════════
// NOTE: Dashboard HTML/CSS/JS are now static files at
//   www/dashboard/index.html   www/dashboard/style.css   www/dashboard/app.js
//   Served by StaticFileHandler. /metrics.json and /metrics/stream
//   remain handled by MetricsMiddleware.
// ═══════════════════════════════════════════════════════════════
