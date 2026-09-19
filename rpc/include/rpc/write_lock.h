// 协程式写互斥：单线程事件循环下的 FIFO 转移式互斥。
// 同一连接的多个协程并发写帧必须经过 WriteLock 串行化，否则 write_all
// 会交错字节（一帧被另一帧打断）。所有权在 Release 时转移给队首等待者，
// 持锁期间 held_ 恒为 true，队列空才置 false。单线程下无锁安全。
#pragma once

#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>

#include "coro/task.h"

namespace rpc {

class WriteLock {
public:
    WriteLock() = default;
    ~WriteLock();
    WriteLock(const WriteLock&) = delete;             // 禁止拷贝
    WriteLock& operator=(const WriteLock&) = delete;  // 禁止拷贝赋值

    // 获取锁：空闲直接持有并返回 true；否则入队等待（FIFO）。
    // timeout_ms < 0 表示无限等待；超时返回 false（不持有锁）
    // 参数：timeout_ms - 等待超时（毫秒，<0 无限）
    coro::Task<bool> Acquire(int64_t timeout_ms);

    // 释放锁：所有权转移给队首等待者并恢复它；队列空则置空闲。
    // 必须由持锁者调用一次
    void Release();

private:
    // 等待者节点：句柄 + 可取消定时器 id（超时判断用）。
    // 用 shared_ptr 持有：节点由队列和等待协程各持一份引用，因此
    // 协程醒来时（无论被 Release 转移还是被定时器恢复）节点必然存活，
    // 可安全遍历队列判断"自己是否还在队列"——避免 unique_ptr 下的悬垂指针比较。
    struct Waiter {
        std::coroutine_handle<> h;
        std::size_t timer_id = 0;  // 0 = 未注册定时器（无限等待）
    };
    // 挂起并注册可取消超时定时器；协程被恢复后自行判断"还在队列 = 超时"
    struct SuspendAwaiter {
        WriteLock* lock;
        Waiter* w;
        int64_t timeout_ms;
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept;
        void await_resume() noexcept {}
    };
    friend struct SuspendAwaiter;

    bool held_ = false;
    std::deque<std::shared_ptr<Waiter>> waiters_;
};

}  // namespace rpc
