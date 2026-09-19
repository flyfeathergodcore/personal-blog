#pragma once
#include "http/server/h11_session.hpp"
#include "net/tls_stream.h"
#include <memory>
#include <vector>

// ── H11Session 对象池 ──
//
// 避免每条新连接反复 make_shared 的开销。
// 池里存空闲的 shared_ptr<H11Session> 壳，
// Server 取出 → Reset(stream) → co_spawn → 用完归还。
//

class H11SessionPool {
public:
    using H11SessionTls = H11Session<net::TlsStream>;

    // 构造函数：创建空对象池
    H11SessionPool() = default;

    /// 取一个空闲 Session（空壳，需 Reset），池空返回 nullptr
    std::shared_ptr<H11SessionTls> TryAcquireSession();

    /// 归还使用完的 Session 至空闲池，供下次连接复用
    void ReleaseSession(std::shared_ptr<H11SessionTls>);

    // 空闲 Session 数量（用于指标/调试）
    // 参数：无；返回：空闲数量
    size_t IdleCount() const;

private:
    std::vector<std::shared_ptr<H11SessionTls>> idle_;
};
