// Task<T>：协程任务原语。非对称模型——协程只能挂起到事件循环，
// 父子协程通过 continuation 链传递结果。
// 帧分配走 FramePool；帧销毁由事件循环统一执行（final_suspend → post_destroy）。
#pragma once

#include <coroutine>
#include <exception>
#include <utility>

#include "coro/event_loop.h"
#include "coro/frame_pool.h"

namespace coro {

template<typename T>
class Task {
public:
    struct promise_type {
        // 协程帧统一从内存池分配
        static void* operator new(std::size_t sz) { return FramePool::alloc(sz); }
        static void operator delete(void* p, std::size_t sz) { FramePool::free(p, sz); }

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() { return {}; }  // 创建即挂起，需手动/co_await 启动

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> self) noexcept {
                auto& prom = self.promise();
                auto cont = prom.continuation_;
                // 先把异常取到局部：post_destroy 仅入队、帧在 drain 时才销毁，
                // 但提前取出更稳妥（notify_error 回调可能在任意时刻执行）
                auto exc = prom.exception_;
                prom.continuation_ = nullptr;
                // 时序关键：必须先 resume(cont) 再 post_destroy(self)。
                // 父协程在 resume 栈内读取子任务结果（await_resume 访问本帧的
                // result_/exception_），必须保证本帧在 resume 期间存活；
                // 若先入销毁队列，多线程 run() 下另一线程可能在本帧恢复父协程
                // 前 drain 销毁队列并 destroy 本帧 → 父协程读取已释放帧（UB）。
                // 销毁投递延迟到 resume 返回之后：单线程下父的读取已完成，
                // 多线程下销毁入队与父读取同帧顺序执行，同样安全。
                if (cont) cont.resume();
                // C2：顶层协程（无 continuation）未捕获异常 → error_handler 兜底，
                // 绝不随帧销毁静默吞没
                if (!cont && exc) EventLoop::current().notify_error(exc);
                EventLoop::current().post_destroy(self);
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T v) { result_ = std::move(v); }
        void unhandled_exception() { exception_ = std::current_exception(); }

        T result_{};
        std::exception_ptr exception_{};
        std::coroutine_handle<> continuation_ = nullptr;
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    // 析构是 no-op：帧生命周期完全由事件循环管理
    ~Task() = default;

    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool valid() const { return h_ != nullptr; }
    std::coroutine_handle<promise_type> handle() const { return h_; }
    // 协程完成后取结果（move）
    T get() { return std::move(h_.promise().result_); }

private:
    std::coroutine_handle<promise_type> h_;
};

// Task<void> 特化
template<>
class Task<void> {
public:
    struct promise_type {
        static void* operator new(std::size_t sz) { return FramePool::alloc(sz); }
        static void operator delete(void* p, std::size_t sz) { FramePool::free(p, sz); }

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> self) noexcept {
                auto& prom = self.promise();
                auto cont = prom.continuation_;
                auto exc = prom.exception_;  // 先取局部，理由同 Task<T>
                prom.continuation_ = nullptr;
                // 与 Task<T> 同理：先 resume(cont) 再 post_destroy(self)，
                // 保证父协程在 resume 栈内读取本帧时帧仍存活，
                // 销毁投递延迟到 resume 返回后，避免多线程 drain 竞态。
                if (cont) cont.resume();
                // C2：顶层协程未捕获异常 → error_handler 兜底
                if (!cont && exc) EventLoop::current().notify_error(exc);
                EventLoop::current().post_destroy(self);
            }
            void await_resume() noexcept {};
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() {}
        void unhandled_exception() { exception_ = std::current_exception(); }

        std::exception_ptr exception_{};
        std::coroutine_handle<> continuation_ = nullptr;
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    ~Task() = default;

    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool valid() const { return h_ != nullptr; }
    std::coroutine_handle<promise_type> handle() const { return h_; }

private:
    std::coroutine_handle<promise_type> h_;
};

// 等待子任务完成：把父协程注册为 continuation，启动子任务
// 注：task_ 用 const 引用——简报原为 Task<T>&，但 co_await AwaitTask<T>{child_task()}
// 传入的是临时对象，非 const 左值引用无法绑定右值（编译错误）。
// const 引用可同时绑定左值/右值；handle()/resume()/promise() 均为 const 可用，
// 语义与简报一致。
template<typename T>
struct AwaitTask {
    const Task<T>& task_;

    bool await_ready() noexcept { return task_.handle().done(); }
    void await_suspend(std::coroutine_handle<> parent) {
        task_.handle().promise().continuation_ = parent;
        task_.handle().resume();  // 启动子任务（initial_suspend 后首次 resume）
    }
    T await_resume() {
        auto& prom = task_.handle().promise();
        if (prom.exception_) std::rethrow_exception(prom.exception_);
        return std::move(prom.result_);
    }
};

// Task<void> 子任务等待特化：Task<void>::promise_type 无 result_ 成员，
// await_resume 只重抛异常（若有），其余与主模板一致
template<>
struct AwaitTask<void> {
    const Task<void>& task_;

    bool await_ready() noexcept { return task_.handle().done(); }
    void await_suspend(std::coroutine_handle<> parent) {
        task_.handle().promise().continuation_ = parent;
        task_.handle().resume();  // 启动子任务（initial_suspend 后首次 resume）
    }
    void await_resume() {
        auto& prom = task_.handle().promise();
        if (prom.exception_) std::rethrow_exception(prom.exception_);
    }
};

// 直接 co_await Task<T>：等价于 co_await AwaitTask<T>{t}。
// 使 TcpStream 等返回 Task<T> 的接口可被协程直接 co_await（Task<void> 走特化）。
template<typename T>
AwaitTask<T> operator co_await(const Task<T>& t) { return AwaitTask<T>{t}; }

}  // namespace coro
