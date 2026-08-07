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

    // 定时器立即到期（ms <= 0）时无需挂起，直接就绪
    bool await_ready() noexcept { return ms_ <= 0; }
    // 挂起：向事件循环注册 ms 毫秒后恢复 h
    // 参数：h - 协程句柄
    void await_suspend(std::coroutine_handle<> h) { loop_->wait_timer(ms_, h); }
    // 恢复后无返回值
    void await_resume() noexcept {}
};

// 生成一个挂起 ms 毫秒的 SleepAwaiter（绑定当前线程事件循环）
// 参数：ms - 休眠毫秒数
// 返回：可 co_await 的 SleepAwaiter
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

    // IO 等待总是先挂起（就绪与否由事件循环判定）
    bool await_ready() noexcept { return false; }
    // 挂起：向事件循环注册 fd 事件等待；可带超时，超时经 timed_out_ 标记
    // 参数：h - 协程句柄
    void await_suspend(std::coroutine_handle<> h) {
        loop_->wait_io(fd_, ev_, h, timeout_ms_, &timed_out_);
    }
    // 恢复后返回就绪/超时结果
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

// 挂起后把句柄暴露给外部：co_await SelfSuspendAwaiter{} 得到 awaiter，
// 外部持 awaiter.h 并在合适时机 h.resume() 唤醒
struct SelfSuspendAwaiter {
    std::coroutine_handle<> h;

    // 总是先挂起，交由外部唤醒
    bool await_ready() noexcept { return false; }
    // 挂起：把父协程句柄存入成员，供外部 h.resume() 唤醒
    // 参数：parent - 父协程句柄
    void await_suspend(std::coroutine_handle<> parent) noexcept { h = parent; }
    // 恢复后无返回值
    void await_resume() noexcept {}
};

}  // namespace coro
