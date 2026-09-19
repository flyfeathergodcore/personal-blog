/*
| 定义 | 做什么 | 典型使用 |
|---|---|---|
| `SleepAwaiter` | 把协程挂起指定毫秒，向事件循环注册定时器。 | `co_await coro::sleep_for(10)` |
| `sleep_for(ms)` | 创建并返回 `SleepAwaiter`，自动关联当前线程的 `EventLoop`。 | 重试退避、定时任务。 |
| `Readiness` | I/O 等待结果。`Ready` 表示 fd 就绪，`Timeout` 表示超时。 | 判断 `await_event` 是否超时。 |
| `AwaitIo` | 等待某个 fd 可读、可写或两者之一，并支持超时。 | socket 的 `accept/read/write/connect`。 |
| `await_event(fd, ev, timeout)` | 创建 `AwaitIo`。`ev` 可以是 `READ`、`WRITE` 或 `READ \| WRITE`。 | `co_await coro::await_event(fd, IoPoller::READ)` |
| `await_readable(fd, timeout)` | `await_event(fd, READ, timeout)` 的简写。 | 等待 socket 可读。 |
| `SelfSuspendAwaiter` | 无条件挂起并保存当前协程句柄，外部代码随后手动恢复它。 | 自定义等待队列、事件通知、窗口更新唤醒。 |
*/
#pragma once

#include <coroutine>
#include <cstdint>

#include "coro/event_loop.h"

namespace coro {

struct SleepAwaiter {
    /*
    成员：
        loop_ - 指向当前线程的事件循环
        ms_   - 挂起毫秒数
    方法：
        await_ready() - 判断是否立即就绪（ms_ <= 0）
        await_suspend(h) - 挂起协程，注册定时器
        await_resume() - 恢复后无返回值
    */
    EventLoop* loop_;
    int64_t ms_;

    bool await_ready() noexcept { return ms_ <= 0; }
    void await_suspend(std::coroutine_handle<> h) { loop_->wait_timer(ms_, h); }
    void await_resume() noexcept {}
};

inline SleepAwaiter sleep_for(int64_t ms) {
    // 生成 SleepAwaiter，绑定当前线程事件循环
    return SleepAwaiter{&EventLoop::current(), ms};
}

enum class Readiness { Ready, Timeout };

// 等待 fd 的事件组合（READ / WRITE 可位或），可带超时
struct AwaitIo {
    /*
    成员：
        fd_ - 文件描述符
        ev_ - 事件组合（READ / WRITE）
        timeout_ms_ - 超时毫秒数（-1 = 无限）
        loop_ - 指向当前线程的事件循环
        timed_out_ - 挂起期间指针有效，标记是否超时
    方法：
        await_ready() - 总是返回 false，表示总是挂起
        await_suspend(h) - 挂起协程，向事件循环注册 fd 事件等待，可带超时
        await_resume() - 恢复后返回就绪/超时结果
    */
    int fd_;
    IoPoller::Event ev_;
    int64_t timeout_ms_;
    EventLoop* loop_;
    bool timed_out_ = false;

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        loop_->wait_io(fd_, ev_, h, timeout_ms_, &timed_out_);
    }
    Readiness await_resume() noexcept {
        return timed_out_ ? Readiness::Timeout : Readiness::Ready;
    }
};

// 生成等待 fd 事件组合的 AwaitIo（绑定当前线程事件循环）
// 参数：fd - 文件描述符；ev - READ/WRITE 事件组合；timeout_ms - 超时毫秒数（-1 = 无限）
// 返回：可 co_await 的 AwaitIo
inline AwaitIo await_event(int fd, IoPoller::Event ev, int64_t timeout_ms = -1) {
    return AwaitIo{fd, ev, timeout_ms, &EventLoop::current(), false};
}

// 等待 fd 可读（await_event 的 READ 特例，原接口不变）
inline AwaitIo await_readable(int fd, int64_t timeout_ms = -1) {
    return await_event(fd, IoPoller::READ, timeout_ms);
}

struct SelfSuspendAwaiter {
    /*
    成员：
        h - 协程句柄，挂起时保存，外部可通过 awaiter.h.resume() 唤醒
    方法：
        await_ready() - 总是返回 false，表示总是挂起
        await_suspend(parent) - 挂起协程，保存句柄
        await_resume() - 恢复后无返回值
    */
    std::coroutine_handle<> h;
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> parent) noexcept { h = parent; }
    void await_resume() noexcept {}
};

}  // namespace coro
