#pragma once
#include <string>
#include <vector>
#include "log/logger.hpp"

struct UpstreamAddr {
    std::string host;
    unsigned short port = 0;
};

struct ProxyRoute {
    std::string prefix;                 // path prefix, e.g. "/api/"
    std::vector<UpstreamAddr> upstreams;
};

struct RedirectRule {
    std::string from;                   // path prefix to match
    std::string to;                     // target URL (Location header)
    int code = 302;                     // 301|302|307|308
};

// MySQL 连接池配置（mysql: 段）。host 默认 host.docker.internal（容器内连宿主机）
struct MysqlConfig {
    std::string host = "host.docker.internal";
    unsigned short port = 3306;
    std::string user;
    std::string password;
    std::string database;
    int min_size = 4;                   // 每 worker 连接池的初始连接数
    int max_size = 16;                  // 上限
};

// 指标参数配置（metrics: 段，作为默认值）。后台「站点设置 → 指标与统计」可在线
// 覆盖并存 MySQL site_config（热生效），重启容器回落本段默认值。
// 注意：与 metrics.hpp 的 RuntimeMetricsConfig、前端 defaultMetricsConfig 保持一致。
struct MetricsConfig {
    int flush_interval_ms      = 1000;   // QPS 刷新周期（Flush 间隔，毫秒，100–1000）
    int persist_interval_secs  = 60;     // 统计落库周期（秒，10–3600）
    int cleanup_interval_secs  = 3600;   // 过期统计清理周期（秒，300–86400）
    int cleanup_retention_days = 30;     // 清理保留窗口（天，7–3650）
    int realtime_refresh_ms    = 3000;   // 前端仪表盘实时轮询（毫秒，1000–60000）
    int trend_refresh_ms       = 60000;  // 前端仪表盘趋势图轮询（毫秒，5000–3600000）
    int visitor_refresh_ms     = 5000;   // 前端仪表盘访问者表轮询（毫秒，1000–60000）
};

struct Config {
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    unsigned short tls_port = 0;         // 0 = TLS disabled
    int threads = 4;
    size_t max_body_size = 0;            // 0 = unlimited (bytes)
    std::string doc_root = "./www";
    std::string tls_cert;                // PEM certificate path
    std::string tls_key;                 // PEM private key path
    bool cpu_affinity = true;            // pin worker threads to dedicated cores
    unsigned int ws_idle_timeout = 0;    // WebSocket 空闲超时（秒，0=不限）

    // Logging
    std::string log_dir = "./logs";      // 日志输出目录
    LogLevel    log_level = LogLevel::Info;  // 日志级别（debug|info|warn|error）

    // Proxy routes
    std::vector<ProxyRoute> proxy_routes;
    // Redirect rules (301/302/307/308)
    std::vector<RedirectRule> redirect_rules;

    // MySQL 连接池配置（博客后端数据源；为空则后端不可用）
    MysqlConfig mysql;

    // 指标参数配置（默认值，见 metrics: 段；后台可在线覆盖热生效）
    MetricsConfig metrics;

    /// Load config from YAML file.  When `strict` is true (used for -t / dry-run),
    /// throws exceptions on parse / file errors instead of silently defaulting.
    static Config Load(const std::string& path, bool strict = false);
};
