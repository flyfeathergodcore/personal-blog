#pragma once
#include "net/tcp_stream.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════
// UpstreamConnPool — 反向代理的上游 TCP 连接复用池（每 worker 一份）
//
// 每个 worker 线程持有自己的 thread_local 池。连接以 (host, port) 标记，
// Acquire() 按目标上游返回空闲 socket。
//
// 空闲连接在下一次 Acquire/Release 时按 kIdleTimeout（30s）淘汰；
// 池容量上限为每 worker kMaxEntries。
//
// 线程安全：thread_local —— 无需加锁。
// ═══════════════════════════════════════════════════════════════════

class UpstreamConnPool {
public:
    /// 一条池化的 TCP 连接（coro/net 版：只持 TcpStream，无 executor）
    struct Conn {
        net::TcpStream socket;
        std::chrono::steady_clock::time_point last_used;
        // socket 是否仍打开
        // 参数：无；返回：是否打开
        bool is_open() const { return socket.is_open(); }
    };

    /// 线程局部单例。
    static UpstreamConnPool& Instance();

    /// 为 (host, port) 获取一条空闲连接；无匹配返回 nullptr。
    std::unique_ptr<Conn> Acquire(const std::string& host,
                                  unsigned short port);

    /// 归还连接以供复用。池接管所有权——调用后勿再使用 `conn`。
    void Release(std::unique_ptr<Conn> conn,
                 const std::string& host,
                 unsigned short port);

    /// 空闲连接数（用于指标/调试）。
    size_t IdleCount() const;

private:
    // 私有构造函数（线程局部单例，禁止外部构造）
    UpstreamConnPool() = default;
    // 析构函数：关闭所有空闲连接
    ~UpstreamConnPool();

    // 禁止拷贝/赋值（线程局部单例）
    UpstreamConnPool(const UpstreamConnPool&) = delete;
    UpstreamConnPool& operator=(const UpstreamConnPool&) = delete;

    // 淘汰过期/失效的空闲连接
    // 参数：无
    void EvictStale();

    struct Entry {
        std::unique_ptr<Conn> conn;
        std::string host;
        unsigned short port;
    };

    std::vector<Entry> entries_;

    static constexpr auto kIdleTimeout = std::chrono::seconds(30);
    static constexpr size_t kMaxEntries = 64;
};
