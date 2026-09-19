// 协程式写互斥实现：见 include/rpc/write_lock.h
#include "rpc/write_lock.h"

#include "coro/event_loop.h"
#include "rpc/timer_id.h"

namespace rpc {

WriteLock::~WriteLock() = default;

// 挂起时注册可取消定时器：到期会恢复本协程，据此判断超时
void WriteLock::SuspendAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    w->h = h;
    if (timeout_ms >= 0) {
        // 可取消定时器 id 全局唯一：避免与 RpcChannel 等组件的 id 在
        // 同一 EventLoop 的 live_timer_ids_ 集合中碰撞
        std::size_t tid = NextTimerId();
        w->timer_id = tid;
        coro::EventLoop::current().wait_timer_cancelable(timeout_ms, h, tid);
    }
}

coro::Task<bool> WriteLock::Acquire(int64_t timeout_ms) {
    // 空闲直接持有
    if (!held_) {
        held_ = true;
        co_return true;
    }
    // 入队等待：本协程与队列各持一份 shared_ptr，节点在挂起期间必然存活
    auto w = std::make_shared<Waiter>();
    Waiter* wp = w.get();
    waiters_.push_back(w);
    co_await SuspendAwaiter{this, wp, timeout_ms};

    // 醒来分两种情况（wp 此时仍有效，本协程持有 shared_ptr）：
    //  - 还在队列：定时器到期（超时），把自己移除并返回 false
    //  - 不在队列：Release 已把所有权转移给我并 resume → 拿到锁
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
        if (it->get() == wp) {
            waiters_.erase(it);
            co_return false;
        }
    }
    co_return true;
}

void WriteLock::Release() {
    // FIFO 转移：给队首等待者（被超时移除的节点已不在队列，无需跳过）
    if (waiters_.empty()) {
        held_ = false;
        return;
    }
    // 先拷出 shared_ptr 再 pop：节点在 pop 后仍存活（等待协程持有一份），
    // 之后的句柄/定时器读取不是悬垂。erase 前的字段读写必须用这份拷贝。
    auto w = waiters_.front();
    waiters_.pop_front();
    std::coroutine_handle<> h = w->h;
    std::size_t tid = w->timer_id;
    // 作废其超时定时器：返回 true 表示取消成功（定时器到期将作废），由本函数
    // resume；返回 false 表示定时器已被到期路径消费（resume 已安排），不能再
    // 手动 resume，否则双 resume 导致协程被恢复两次。
    bool should_resume = true;
    if (tid != 0) should_resume = coro::EventLoop::current().cancel_timer(tid);
    if (should_resume) h.resume();
    // 所有权转移完成：held_ 保持 true，由新持有者释放
}

}  // namespace rpc
