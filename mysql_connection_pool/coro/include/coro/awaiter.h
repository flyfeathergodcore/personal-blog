// 常用 Awaitable：sleep_for、await_event（泛化 IO 等待）、SelfSuspendAwaiter（外部唤醒挂起）
#pragma once

#include <coroutine>
#include <cstdint>

#include "coro/event_loop.h"

namespace coro {

// 定时器挂起
struct SleepAwaiter {
    EventLoop* loop_;
    int64_t ms_;

    bool await_ready() noexcept { return ms_ <= 0; }
    void await_suspend(std::coroutine_handle<> h) { loop_->wait_timer(ms_, h); }
    void await_resume() noexcept {}
};

inline SleepAwaiter sleep_for(int64_t ms) {
    return SleepAwaiter{&EventLoop::current(), ms};
}

// IO 就绪等待结果
enum class Readiness { Ready, Timeout };

// 等待 fd 的事件组合（READ / WRITE 可位或），可带超时
struct AwaitIo {
    int fd_;
    IoPoller::Event ev_;
    int64_t timeout_ms_;
    EventLoop* loop_;
    bool timed_out_ = false;  // 存在协程帧里，挂起期间指针有效

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        loop_->wait_io(fd_, ev_, h, timeout_ms_, &timed_out_);
    }
    Readiness await_resume() noexcept {
        return timed_out_ ? Readiness::Timeout : Readiness::Ready;
    }
};

inline AwaitIo await_event(int fd, IoPoller::Event ev, int64_t timeout_ms = -1) {
    return AwaitIo{fd, ev, timeout_ms, &EventLoop::current(), false};
}

// 等待 fd 可读（await_event 的 READ 特例，原接口不变）
inline AwaitIo await_readable(int fd, int64_t timeout_ms = -1) {
    return await_event(fd, IoPoller::READ, timeout_ms);
}

// 挂起后把句柄暴露给外部：co_await SelfSuspendAwaiter{} 得到 awaiter，
// 外部持 awaiter.h 并在合适时机 h.resume() 唤醒
struct SelfSuspendAwaiter {
    std::coroutine_handle<> h;

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> parent) noexcept { h = parent; }
    void await_resume() noexcept {}
};

}  // namespace coro
