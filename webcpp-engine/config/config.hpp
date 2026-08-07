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

    /// Load config from YAML file.  When `strict` is true (used for -t / dry-run),
    /// throws exceptions on parse / file errors instead of silently defaulting.
    static Config Load(const std::string& path, bool strict = false);
};
