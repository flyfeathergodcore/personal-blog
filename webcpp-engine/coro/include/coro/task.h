// Task<T>：协程任务原语。非对称模型——协程只能挂起到事件循环，
// 父子协程通过 continuation 链传递结果。
// 帧分配走 FramePool；帧销毁由事件循环统一执行（final_suspend → post_destroy）。
#pragma once

#include <coroutine>
#include <exception>
#include <new>
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
        // 释放协程帧（经内存池释放）
        static void operator delete(void* p, std::size_t sz) { FramePool::free(p, sz); }
        static void* operator new(std::size_t sz, std::align_val_t align) {
            return FramePool::alloc_aligned(sz, static_cast<std::size_t>(align));
        }
        static void operator delete(void* p, std::size_t sz, std::align_val_t) {
            FramePool::free(p, sz);
        }

        // 创建 Task 句柄对象返回给调用方
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        // 创建即挂起，需手动/co_await 启动
        std::suspend_always initial_suspend() { return {}; }

        struct FinalAwaiter {
            // 总是先挂起，由 await_suspend 统一收尾
            bool await_ready() noexcept { return false; }
            // 协程结束挂起：恢复父协程（continuation）并投递本帧统一销毁；顶层异常交给 error_handler 兜底
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
            // 恢复后无返回值
            void await_resume() noexcept {}
        };
        // 最终挂起点：返回 FinalAwaiter 统一收尾
        FinalAwaiter final_suspend() noexcept { return {}; }

        // 协程返回值：保存结果供父协程 await_resume 读取
        void return_value(T v) { result_ = std::move(v); }
        // 协程体未捕获异常：保存到异常槽供父协程重抛
        void unhandled_exception() { exception_ = std::current_exception(); }

        T result_{};
        std::exception_ptr exception_{};
        std::coroutine_handle<> continuation_ = nullptr;
    };

    // 默认构造：不持有任何协程句柄
    Task() = default;
    // 由协程句柄构造
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    // 析构是 no-op：帧生命周期完全由事件循环管理
    ~Task() = default;

    // 移动构造：转移协程句柄所有权
    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    // 移动赋值：转移协程句柄所有权，处理自赋值
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    Task(const Task&) = delete;  // 禁止拷贝
    Task& operator=(const Task&) = delete;  // 禁止拷贝赋值

    // 判断是否持有有效协程句柄
    bool valid() const { return h_ != nullptr; }
    // 获取底层协程句柄
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
        static void* operator new(std::size_t sz, std::align_val_t align) {
            return FramePool::alloc_aligned(sz, static_cast<std::size_t>(align));
        }
        static void operator delete(void* p, std::size_t sz, std::align_val_t) {
            FramePool::free(p, sz);
        }

        // 创建 Task 句柄对象返回给调用方
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        // 创建即挂起，需手动/co_await 启动
        std::suspend_always initial_suspend() { return {}; }

        struct FinalAwaiter {
            // 总是先挂起，由 await_suspend 统一收尾
            bool await_ready() noexcept { return false; }
            // 协程结束挂起：恢复父协程并投递本帧统一销毁；顶层异常交给 error_handler 兜底
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
            // 恢复后无返回值
            void await_resume() noexcept {};
        };
        // 最终挂起点：返回 FinalAwaiter 统一收尾
        FinalAwaiter final_suspend() noexcept { return {}; }

        // 协程正常结束：void 任务无返回值
        void return_void() {}
        // 协程体未捕获异常：保存到异常槽供父协程重抛
        void unhandled_exception() { exception_ = std::current_exception(); }

        std::exception_ptr exception_{};
        std::coroutine_handle<> continuation_ = nullptr;
    };

    // 默认构造：不持有任何协程句柄
    Task() = default;
    // 由协程句柄构造
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    // 析构是 no-op：帧生命周期完全由事件循环管理
    ~Task() = default;

    // 移动构造：转移协程句柄所有权
    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    // 移动赋值：转移协程句柄所有权，处理自赋值
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    Task(const Task&) = delete;  // 禁止拷贝
    Task& operator=(const Task&) = delete;  // 禁止拷贝赋值

    // 判断是否持有有效协程句柄
    bool valid() const { return h_ != nullptr; }
    // 获取底层协程句柄
    std::coroutine_handle<promise_type> handle() const { return h_; }

private:
    std::coroutine_handle<promise_type> h_;
};

// 等待子任务完成：把父协程注册为 continuation，启动子任务。
// 注：AwaitTask 只保存 coroutine_handle（挂起前拷贝），不保存 Task 引用——
// co_await child() 这类表达式把 Task 临时对象传给 operator co_await，临时对象
// 的生命周期只到完整表达式结束；若像旧实现那样存 const Task<T>&，协程挂起后
// 引用即悬垂（UB：GCC 因实现细节恰好不崩，clang 严格按标准会在 await_resume
// 读已释放内存而崩，实测 SignalWatcher::wait 崩）。Task 析构是 no-op（帧由
// 事件循环管理），拷贝出的 handle 在挂起期间始终有效，跨编译器安全。
template<typename T>
struct AwaitTask {
    using promise_t = typename Task<T>::promise_type;
    std::coroutine_handle<promise_t> h_;

    // 构造函数：拷贝子任务句柄（不持有 Task 引用，避免挂起后悬垂）
    explicit AwaitTask(const Task<T>& t) : h_(t.handle()) {}

    // 子任务已完成则直接就绪（无需挂起）
    bool await_ready() noexcept { return h_.done(); }
    // 挂起：把父协程注册为子任务 continuation 并启动子任务
    // 参数：parent - 父协程句柄
    void await_suspend(std::coroutine_handle<> parent) {
        h_.promise().continuation_ = parent;
        h_.resume();  // 启动子任务（initial_suspend 后首次 resume）
    }
    // 恢复后：子任务有异常则重抛，否则返回结果
    T await_resume() {
        auto& prom = h_.promise();
        if (prom.exception_) std::rethrow_exception(prom.exception_);
        return std::move(prom.result_);
    }
};

// Task<void> 子任务等待特化：Task<void>::promise_type 无 result_ 成员，
// await_resume 只重抛异常（若有），其余与主模板一致
template<>
struct AwaitTask<void> {
    using promise_t = typename Task<void>::promise_type;
    std::coroutine_handle<promise_t> h_;

    // 构造函数：拷贝子任务句柄（不持有 Task 引用，避免挂起后悬垂）
    explicit AwaitTask(const Task<void>& t) : h_(t.handle()) {}

    // 子任务已完成则直接就绪（无需挂起）
    bool await_ready() noexcept { return h_.done(); }
    // 挂起：把父协程注册为子任务 continuation 并启动子任务
    // 参数：parent - 父协程句柄
    void await_suspend(std::coroutine_handle<> parent) {
        h_.promise().continuation_ = parent;
        h_.resume();  // 启动子任务（initial_suspend 后首次 resume）
    }
    // 恢复后：子任务有异常则重抛
    void await_resume() {
        auto& prom = h_.promise();
        if (prom.exception_) std::rethrow_exception(prom.exception_);
    }
};

// 直接 co_await Task<T>：等价于 co_await AwaitTask<T>{t}。
// 使 TcpStream 等返回 Task<T> 的接口可被协程直接 co_await（Task<void> 走特化）。
template<typename T>
AwaitTask<T> operator co_await(const Task<T>& t) { return AwaitTask<T>{t}; }

}  // namespace coro
