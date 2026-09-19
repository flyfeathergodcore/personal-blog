# webcpp-engine

基于 coroutine 的 C++ HTTP/1.1 + HTTP/2 服务器。事件循环使用随工程分发的
`coro/`（epoll ET 模式，每 worker 一个独立 EventLoop），零 asio 依赖。

目录按职责分层：`net/` 提供 TCP/TLS、监听、解析地址与缓冲 I/O；`tcp/` 提供可复用
的 `tcp::Reactor`（Linux epoll / macOS kqueue）、`tcp::Listener`、带 `Receive` /
`Send` / `Sendv` 的 `tcp::Channel`、`tcp::Server` 与服务入口；`http/` 提供 HTTP
协议、会话、路由、中间件、配置和 handler，并链接 `webcpp_tcp`。
原有 `webcpp_engine` CMake 目标保留为兼容入口，链接到 `webcpp_http` 即可。

## 构建与运行

```bash
# 构建（默认使用当前工程的 coro/）
cmake -B build && cmake --build build -j

# 运行（明文 h1）
./build/demo_server -c /tmp/perf_plain_cfg.yaml   # port 8082, threads 4

# 运行（TLS h2）—— tls_port > 0 时单端口只监听 TLS，h1/h2 需两个进程
./build/demo_server -c /tmp/perf_tls_cfg.yaml     # tls_port 8443

# 独立 TCP 字节回显服务（默认 127.0.0.1:8081）
./build/tcp_server -p 8081
```

配置文件为 YAML，`server:` 段支持 `port` / `tls_port` / `threads` / `log_dir` /
`log_level` / `doc_root` / `tls_cert` / `tls_key` / `cpu_affinity`。

## 测试

```bash
ctest --test-dir build  # 单元测试与 HTTP/TCP 冒烟测试
```

## 性能

三层日志（Gateway/Business/Perf）+ 无锁异步写；日志 accessor 用 magic-static 惰性初始化
（无每请求全局锁）、时间戳秒级前缀按线程缓存、Gateway 直接 string_view 写 char buf。

### 与 nginx 对比（2026-08-06，修复后）

环境：4 核 Linux 6.8，webcpp 与 nginx 1.24 各 4 worker，同机同内容
（`www/index.html`，21B）。h1 用 `wrk -t4`，h2 用 `h2load`（m=10 多路复用，
`SSL_CERT_FILE` 信任自签证书，定长压测），协议完全对齐（h2 双方均走 TLS/ALPN）。

| 协议 | 参数 | webcpp | nginx | webcpp/nginx |
|------|------|--------|-------|--------------|
| h1 明文 | c=200 | **94.8–95.1k** req/s | 93.1–94.2k | **100.6–102.1%**（反超） |
| h1 明文 | c=1000 | 88.2k | 95.5k | **92.4%** |
| h2 TLS | c=200, m=10 | **136.5–137.3k** req/s | 65.7–68.1k | **恰 2.0×** |
| h2 TLS | c=50, m=10 | 130.5k | 69.3k | **1.8×** |

- 200 万 h2 请求全部成功（0 failed / 0 errored）。
- 修复前 h1 c=200 仅 52100 req/s（nginx 的 59%）；唤醒+日志两处修复后
  **h1 与 nginx 打平（低并发反超）、h2 为 nginx 的 2 倍**。
- 注：nginx h2 默认 `keepalive_requests 1000` 会限制 HTTP/2 每连接请求数，对比测试
  需调大（`keepalive_requests 100000`），否则高压力下会 GOAWAY 断连误判为失败。

### 已落地的性能修复

1. **唤醒管道"大水漫灌"根治**（coro event_loop，随库传播）：`wake()` 从写满 64KB 管道
   改为单块 64 字节；新增 `waiters_` 计数，仅在确有线程阻塞于 poll 时按需唤醒。
   修复前空闲时每 worker 每秒 ~1026 次管道读写（纯空转），修复后 0/0。
2. **日志 26% 开销根治**（log/logger.hpp + http/middleware/middleware.cpp）：每请求全局锁、
   重复 localtime_r/strftime、多次堆分配 全部消除（详情见代码注释与 SDD 台账）。
