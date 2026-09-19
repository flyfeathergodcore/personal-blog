// 客户端连接池实现：见 include/rpc/rpc_pool.h
#include "rpc/rpc_pool.h"

#include "coro/awaiter.h"
#include "coro/event_loop.h"
#include "rpc/timer_id.h"

namespace rpc {

RpcConnectionPool::RpcConnectionPool(std::size_t max) : max_(max) {}
RpcConnectionPool::~RpcConnectionPool() = default;

// 挂起入队并注册可取消超时定时器：到期恢复本协程，据此判断超时
void RpcConnectionPool::WaitAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    w->h = h;
    if (timeout_ms >= 0) {
        std::size_t tid = NextTimerId();
        w->timer_id = tid;
        coro::EventLoop::current().wait_timer_cancelable(timeout_ms, h, tid);
    }
}

coro::Task<std::shared_ptr<RpcChannel>> RpcConnectionPool::Acquire(
    std::string_view host, std::uint16_t port, int64_t timeout_ms) {
    for (;;) {
        // 1. 空闲连接优先复用；已失效（读循环退出）则丢弃后回循环顶重试
        if (!idle_.empty()) {
            auto c = std::move(idle_.front());
            idle_.pop_front();
            if (!c->read_done()) co_return c;
            --total_;  // 失效连接不计入池（读循环退出即连接已断，无需再 Close）
            continue;
        }
        // 2. 未达上限：新建连接
        if (total_ < max_) {
            ++total_;
            auto ch = std::make_shared<RpcChannel>();
            if (co_await ch->Open(host, port, timeout_ms)) co_return ch;
            --total_;  // 连接失败，不计入
            co_return nullptr;
        }
        // 3. 已满员：FIFO 入队等待（本协程与队列各持一份 shared_ptr，挂起期间节点必然存活）
        auto w = std::make_shared<Waiter>();
        Waiter* wp = w.get();
        waiters_.push_back(w);
        co_await WaitAwaiter{this, wp, timeout_ms};

        // 醒来分三类（wp 此时仍有效，本协程持有 shared_ptr）：
        //  - w->conn 非空：Release 已移交连接 → 直接返回
        //  - 还在队列：定时器到期（超时）→ 移出队列返回空
        //  - 不在队列且 conn 空：Release 释放了名额（丢弃/空归还）→ 回循环顶重试
        if (w->conn) co_return std::move(w->conn);
        bool timed_out = false;
        for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
            if (it->get() == wp) {
                waiters_.erase(it);
                timed_out = true;
                break;
            }
        }
        if (timed_out) co_return nullptr;
    }
}

void RpcConnectionPool::Release(const std::shared_ptr<RpcChannel>& conn, bool healthy) {
    if (!conn) {
        WakeNext();  // 空归还 = 仅释放一个名额（罕见，防御性处理）
        return;
    }
    if (!healthy) {
        conn->Close();
        --total_;   // 归还的失效连接不再计池：配对的是 Acquire 新建时的 ++total_
        WakeNext();  // 释放名额：唤醒队首等待者回循环顶重试（新建或复用空闲）
        return;
    }
    // 有等待者 → 直接移交，不入空闲队列（避免额外转手）
    if (!waiters_.empty()) {
        auto w = waiters_.front();
        waiters_.pop_front();
        w->conn = conn;
        WakeOne(w);
        return;
    }
    idle_.push_back(conn);
}

// 关闭全部空闲连接并等待读循环退出：逐个 Close + 轮询 read_done，
// 全部退出后 size() 归零，池可安全析构。
coro::Task<void> RpcConnectionPool::CloseAll() {
    while (!idle_.empty()) {
        auto c = std::move(idle_.front());
        idle_.pop_front();
        c->Close();
        while (!c->read_done()) co_await coro::sleep_for(10);
        --total_;
    }
    co_return;
}

// 作废定时器并恢复协程：cancel_timer 返回 true 表示取消成功（定时器不会再触发），
// 由本函数手动 resume；false 表示定时器到期路径已消费（resume 已安排），
// 不得再手动 resume——否则协程被恢复两次（双 resume）。
void RpcConnectionPool::WakeOne(const std::shared_ptr<Waiter>& w) {
    bool should_resume = true;
    if (w->timer_id != 0) should_resume = coro::EventLoop::current().cancel_timer(w->timer_id);
    if (should_resume) w->h.resume();
}

void RpcConnectionPool::WakeNext() {
    if (waiters_.empty()) return;
    auto w = waiters_.front();
    waiters_.pop_front();
    WakeOne(w);
}

}  // namespace rpc
