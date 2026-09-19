#include "http/server/h11_session_pool.hpp"

// 从空闲池取一个 Session 空壳（需调用方 Reset 后使用）；池空返回 nullptr
// 参数：无；返回：空闲 Session 或 nullptr
std::shared_ptr<H11SessionPool::H11SessionTls>
H11SessionPool::TryAcquireSession()
{
    if (!idle_.empty()) {
        auto s = std::move(idle_.back());
        idle_.pop_back();
        return s;
    }
    return nullptr;
}

// 归还使用完的 Session 到空闲池，供下次连接复用
// 参数：session - 待归还的 Session（shared_ptr）
void H11SessionPool::ReleaseSession(
    std::shared_ptr<H11SessionTls> session)
{
    idle_.push_back(std::move(session));
}

// 返回当前空闲 Session 数量（用于指标/调试）
// 参数：无；返回：空闲数量
size_t H11SessionPool::IdleCount() const
{
    return idle_.size();
}
