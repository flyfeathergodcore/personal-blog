// ═══════════════════════════════════════════════════════════════════
// mysql_handler — 博客后端 REST API（登录 / 文章 / 分类 / 资源 / 上传）
//
// 数据访问基于 mysql_connection_pool 异步连接池（coro 协程）：
//   - 每 worker 线程一个连接池（thread_local，见 mysql_handler.cpp）
//   - 接口全走异步路径（IsAsync + HandleAsync），不阻塞事件循环
//
// 用法（demo_server.cpp）：
//   if (!cfg.mysql.database.empty()) RegisterBlogRoutes(router, cfg.mysql);
// ═══════════════════════════════════════════════════════════════════
#pragma once
#include "config/config.hpp"
#include "middleware/middleware.hpp"
#include <string>

class Router;

// 注册全部博客 REST 路由（/api/login、/api/articles...）。
// handler 内部持有 cfg 的引用，cfg 的生命周期必须比 Router 长（demo_server 的
// Config 存活到 server.Start() 返回，满足该约束）。
void RegisterBlogRoutes(Router& router, const MysqlConfig& cfg);

// ── 局域网访问开关（后台「工作区 → 局域网访问」） ──
//
// 应用层拦截方案：webcpp 容器在 Docker 内无法区分请求来源（NAT 后源 IP 全是
// bridge 地址），改用「请求 Host 头」判断——关闭时，凡 Host 为局域网 IP
// （非 localhost/127.0.0.1/0.0.0.0）的请求一律返回 403，本机后台不受影响。
// 端口保持 0.0.0.0（docker -p 8443:8443），无需重建容器，零断线风险。

// 启动时（main 线程，同步 MySQL）读 site_config 初始化开关状态：
//   - 建表 site_config（CREATE TABLE IF NOT EXISTS，不依赖不可重跑的 init.sql）
//   - 读 lan_enabled 到内存；表/行不存在时保持默认（关闭）
//   - 读取环境变量 HOST_LAN_IP 作为宿主机局域网 IP（build-run.sh 注入）
void InitLanStateFromDb(const MysqlConfig& cfg);

// 启动时（main 线程，同步 MySQL）从 site_config(key='metrics_config') 读回
// 后台在线覆盖的指标参数并热应用（覆盖 yaml 默认值）；表/行不存在或
// 连接失败时静默回落 yaml 默认值，不阻塞启动。
void InitMetricsConfigFromDb(const MysqlConfig& cfg);

// 当前开关状态 / 宿主机局域网 IP（中间件、handler、demo_server 共用）
bool IsLanEnabled();
std::string LanIp();

// 关闭时拦截局域网 IP Host 请求的中间件（localhost 放行）。
// 注册到 MiddlewareManager 的 PreRequest 阶段，返回 403「局域网访问已关闭」。
class LanGuardMiddleware : public Middleware {
public:
    // 返回中间件阶段：PreRequest
    Type GetType() const override { return Type::PreRequest; }
    // 拦截局域网 IP Host 的请求并返回 403（localhost 放行）
    // 参数：ctx - 请求上下文
    Response HandlePre(Context& ctx) override;
};

// 访问者在线追踪中间件：每个请求 Pre 阶段用 ctx.PeerIp() 刷新该 IP 的最近活跃
// 时间（在线判定依据）。注册到 MiddlewareManager 的 PreRequest 阶段。
class VisitorTrackMiddleware : public Middleware {
public:
    // 返回中间件阶段：PreRequest
    Type GetType() const override { return Type::PreRequest; }
    // 刷新请求方 IP 的活跃时间；放行请求
    // 参数：ctx - 请求上下文（含对端 IP）
    Response HandlePre(Context& ctx) override;
};

class MetricsCollector;
namespace coro { template <typename T> class Task; }

// 每 60s 落库协程：聚合 MetricsCollector 最近 60s → INSERT site_stats（分钟粒度）。
// 由 MultiServer 的 persist 回调调用，运行在 worker 0 的 event loop 线程
//（GetPool 是 thread_local，此处即 worker0 的连接池）。
coro::Task<void> PersistSiteStats(MetricsCollector* mc, const MysqlConfig& cfg);

// 清理过期统计协程：删除 site_stats 中早于保留窗口的记录（retention_days 天），
// 防止历史统计无限增长。与 PersistSiteStats 同线程调用（worker 0），
// 通常按 cleanup_interval_secs 周期执行。参数：cfg - MySQL 连接配置；
// retention_days - 保留窗口天数（调用方已 clamp 到 [7,3650]，无注入面）
coro::Task<void> CleanupSiteStats(const MysqlConfig& cfg, int retention_days);
