#include "config/config.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <stdexcept>

namespace {

// 解析日志级别字符串；成功返回 true 并写入 out
// 参数：s - 日志级别字符串；out - 输出解析结果
bool ParseLogLevel(const std::string& s, LogLevel& out)
{
    if (s == "debug") { out = LogLevel::Debug; return true; }
    if (s == "info")  { out = LogLevel::Info;  return true; }
    if (s == "warn")  { out = LogLevel::Warn;  return true; }
    if (s == "error") { out = LogLevel::Error; return true; }
    return false;
}

} // namespace

// 从 YAML 文件加载配置；strict 为 true 时解析/文件错误直接抛出
// 参数：path - 配置文件路径；strict - 是否严格模式（-t / dry-run）；返回解析后的配置
Config Config::Load(const std::string& path, bool strict)
{
    Config cfg;

    try {
        YAML::Node root = YAML::LoadFile(path);
        auto srv = root["server"];
        // 兼容扁平格式：没有 server: 包裹时直接用顶层字段（demo_smoke 使用）
        if (!srv) srv = root;

        if (srv["host"])       cfg.host     = srv["host"].as<std::string>();
        if (srv["port"])       cfg.port     = srv["port"].as<unsigned short>();
        if (srv["tls_port"])   cfg.tls_port = srv["tls_port"].as<unsigned short>();
        if (srv["threads"])    cfg.threads  = srv["threads"].as<int>();

        // 钳制 worker 线程数下限：threads: 0 / 负值会让 MultiServer::Start 里
        // workers_ 为空，随后 workers_[0] 越界访问（UB）。配置非法时按 1 处理。
        if (cfg.threads < 1) {
            std::cerr << "[config] threads=" << cfg.threads
                      << " 非法，已钳制为 1" << std::endl;
            cfg.threads = 1;
        }
        if (srv["doc_root"])     cfg.doc_root     = srv["doc_root"].as<std::string>();
        if (srv["cpu_affinity"]) cfg.cpu_affinity = srv["cpu_affinity"].as<bool>();
        if (srv["max_body_size"]) cfg.max_body_size = srv["max_body_size"].as<size_t>();
        if (srv["ws_idle_timeout"]) cfg.ws_idle_timeout = srv["ws_idle_timeout"].as<unsigned int>();

        auto tls = srv["tls"];
        if (tls) {
            if (tls["cert"])  cfg.tls_cert = tls["cert"].as<std::string>();
            if (tls["key"])   cfg.tls_key  = tls["key"].as<std::string>();
        }
        // 兼容扁平格式的 tls_cert/tls_key（demo_smoke 使用）
        if (srv["tls_cert"])  cfg.tls_cert = srv["tls_cert"].as<std::string>();
        if (srv["tls_key"])   cfg.tls_key  = srv["tls_key"].as<std::string>();

        // ── Logging (log: { dir, level }) ──
        auto log = root["log"];
        if (log) {
            if (log["dir"]) cfg.log_dir = log["dir"].as<std::string>();
            if (log["level"]) {
                LogLevel lvl;
                auto lvl_s = log["level"].as<std::string>();
                if (ParseLogLevel(lvl_s, lvl)) {
                    cfg.log_level = lvl;
                } else if (strict) {
                    throw std::runtime_error("未知日志级别: " + lvl_s);
                } else {
                    std::cerr << "[config] 无效日志级别 '" << lvl_s
                              << "'，使用默认 info" << std::endl;
                }
            }
        }

        // ── MySQL (mysql: { host, port, user, password, database, min_size, max_size }) ──
        auto mysql = root["mysql"];
        if (mysql) {
            if (mysql["host"])     cfg.mysql.host     = mysql["host"].as<std::string>();
            if (mysql["port"])     cfg.mysql.port     = mysql["port"].as<unsigned short>();
            if (mysql["user"])     cfg.mysql.user     = mysql["user"].as<std::string>();
            if (mysql["password"]) cfg.mysql.password = mysql["password"].as<std::string>();
            if (mysql["database"]) cfg.mysql.database = mysql["database"].as<std::string>();
            if (mysql["min_size"]) cfg.mysql.min_size = mysql["min_size"].as<int>();
            if (mysql["max_size"]) cfg.mysql.max_size = mysql["max_size"].as<int>();
        }

        // ── Metrics (metrics: { flush_interval_ms, persist_interval_secs, ... }) ──
        // 指标参数默认值；后台「站点设置」可在线覆盖并存 site_config，重启回落这里。
        auto metrics = root["metrics"];
        if (metrics) {
            if (metrics["flush_interval_ms"])      cfg.metrics.flush_interval_ms      = metrics["flush_interval_ms"].as<int>();
            if (metrics["persist_interval_secs"])  cfg.metrics.persist_interval_secs  = metrics["persist_interval_secs"].as<int>();
            if (metrics["cleanup_interval_secs"])  cfg.metrics.cleanup_interval_secs  = metrics["cleanup_interval_secs"].as<int>();
            if (metrics["cleanup_retention_days"]) cfg.metrics.cleanup_retention_days = metrics["cleanup_retention_days"].as<int>();
            if (metrics["realtime_refresh_ms"])    cfg.metrics.realtime_refresh_ms    = metrics["realtime_refresh_ms"].as<int>();
            if (metrics["trend_refresh_ms"])       cfg.metrics.trend_refresh_ms       = metrics["trend_refresh_ms"].as<int>();
            if (metrics["visitor_refresh_ms"])     cfg.metrics.visitor_refresh_ms     = metrics["visitor_refresh_ms"].as<int>();
        }

        // ── Proxy routes ──
        auto proxy = root["proxy"];
        if (proxy && proxy.IsSequence()) {
            for (size_t i = 0; i < proxy.size(); i++) {
                auto route = proxy[i];
                ProxyRoute pr;
                if (route["prefix"]) pr.prefix = route["prefix"].as<std::string>();

                auto parse_addr = [](const std::string& s) -> UpstreamAddr {
                    UpstreamAddr a;
                    auto colon = s.rfind(':');
                    if (colon != std::string::npos) {
                        a.host = s.substr(0, colon);
                        a.port = static_cast<unsigned short>(
                            std::stoi(s.substr(colon + 1)));
                    } else {
                        a.host = s;
                        a.port = 80;
                    }
                    return a;
                };

                // New format: upstreams (list)
                if (route["upstreams"] && route["upstreams"].IsSequence()) {
                    for (size_t j = 0; j < route["upstreams"].size(); j++) {
                        auto addr_s = route["upstreams"][j].as<std::string>();
                        pr.upstreams.push_back(parse_addr(addr_s));
                    }
                }
                // Old format: single upstream (string)
                else if (route["upstream"]) {
                    auto addr_s = route["upstream"].as<std::string>();
                    pr.upstreams.push_back(parse_addr(addr_s));
                }

                if (!pr.prefix.empty() && !pr.upstreams.empty()) {
                    cfg.proxy_routes.push_back(std::move(pr));
                }
            }
        }

        // ── Redirect rules ──
        auto redir = root["redirect"];
        if (redir && redir.IsSequence()) {
            for (size_t i = 0; i < redir.size(); i++) {
                auto r = redir[i];
                RedirectRule rr;
                if (r["from"]) rr.from = r["from"].as<std::string>();
                if (r["to"])   rr.to   = r["to"].as<std::string>();
                if (r["code"]) rr.code = r["code"].as<int>();
                if (!rr.from.empty() && !rr.to.empty())
                    cfg.redirect_rules.push_back(std::move(rr));
            }
        }

        std::cout << "[config] 加载 " << path << std::endl;
        if (!cfg.proxy_routes.empty())
            std::cout << "[config] " << cfg.proxy_routes.size()
                      << " 代理路由已配置" << std::endl;
        if (!cfg.redirect_rules.empty())
            std::cout << "[config] " << cfg.redirect_rules.size()
                      << " 重定向规则已配置" << std::endl;
    } catch (std::exception& e) {
        if (strict) throw;  // dry-run mode: propagate error
        std::cerr << "[config] 加载失败，使用默认配置: " << e.what() << std::endl;
    }

    return cfg;
}
