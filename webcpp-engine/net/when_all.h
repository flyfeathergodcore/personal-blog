// net 层并发原语：spawn 投递任务到事件循环；when_all 并发等待两个子任务。
// 与任务简报的 when_all 有一处必要偏差（简报缺陷，否则简报自身 Step 4 测试无法编译）：
//   简报签名 `when_all(A a, B b)` 把模板参数 A/B 直接推为入参类型本身——
//   调用 `when_all(wc_slow(), wc_fast())`（均为 coro::Task<int>）时 A=Task<int>，
//   于是 Slot<A> 存的是 Task、run_one<A> 期望 Task<Task<A>>，双双编译失败。
//   本实现把形参写作 `coro::Task<A> a, coro::Task<B> b`，A/B 即子任务的结果值类型
//   （与 when_all_void 的显式 `coro::Task<void>` 形参风格一致），返回 tuple<A,B> 正确。
#pragma once
#include "coro/task.h"
#include "coro/awaiter.h"
#include <atomic>
#include <exception>
#include <memory>
#include <tuple>
#include <utility>

namespace net {

// 把任务投递到指定事件循环执行
// 参数：task - 待执行任务（所有权转移）；loop - 目标事件循环
inline void spawn(coro::Task<void> task, coro::EventLoop& loop) {
    loop.post(task.handle());
}
// 把任务投递到当前线程的事件循环执行
// 参数：task - 待执行任务（所有权转移）
inline void spawn(coro::Task<void> task) {
    coro::EventLoop::current().post(task.handle());
}

namespace detail {
template<typename T> struct Slot { T value{}; std::exception_ptr exc; };
struct JoinCtl { std::atomic<int> remaining{0}; std::coroutine_handle<> parent; };
struct JoinAwaiter {
    JoinCtl* ctl;
    // co_await 总是挂起（等待两个子任务完成）
    bool await_ready() noexcept { return false; }
    // 挂起时记录父协程句柄，供最后一个子任务完成时恢复
    void await_suspend(std::coroutine_handle<> h) noexcept { ctl->parent = h; }
    // 恢复时无返回值
    void await_resume() noexcept {}
};
// 运行单个子任务并把结果/异常写入共享槽；全部完成后投递父协程。
// 不能在最后一个子协程的完成栈内直接 resume 父协程：父协程可能随即完成
// 并销毁控制块。投递可使最后一个子协程先退出使用控制块的代码路径，
// 再由父协程读取结果并销毁其唯一所有者。
// 参数：task - 子任务（所有权转移）；slot - 结果槽；ctl - 共享完成计数
template<typename T>
coro::Task<void> run_one(coro::Task<T> task, detail::Slot<T>* slot,
                         detail::JoinCtl* ctl) {
    try {
        slot->value = co_await coro::AwaitTask<T>{task};
    } catch (...) {
        slot->exc = std::current_exception();
    }
    if (ctl->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto parent = ctl->parent;
        ctl->parent = nullptr;
        coro::EventLoop::current().post(parent);
    }
}
}  // namespace detail

// 并发执行两个子任务，都完成后返回结果元组；任一异常重抛
// 参数：a, b - 子任务（所有权转移）。返回 std::tuple<A, B>
template<typename A, typename B>
coro::Task<std::tuple<A, B>> when_all(coro::Task<A> a, coro::Task<B> b) {
    std::tuple<detail::Slot<A>, detail::Slot<B>> slots;
    // The parent frame owns the control block. It cannot be destroyed until
    // both children decrement remaining and the parent is posted again.
    auto ctl = std::make_unique<detail::JoinCtl>();
    ctl->remaining.store(2);
    auto& sa = std::get<0>(slots);
    auto& sb = std::get<1>(slots);
    coro::EventLoop& loop = coro::EventLoop::current();
    spawn(detail::run_one<A>(std::move(a), &sa, ctl.get()), loop);
    spawn(detail::run_one<B>(std::move(b), &sb, ctl.get()), loop);
    co_await detail::JoinAwaiter{ctl.get()};
    if (sa.exc) std::rethrow_exception(sa.exc);
    if (sb.exc) std::rethrow_exception(sb.exc);
    co_return std::make_tuple(std::move(sa.value), std::move(sb.value));
}

// void 特化：并发跑两个 void 任务，异常重抛，正常返回 true（WS 双向 relay 用）
namespace detail {
struct VoidSlot { std::exception_ptr exc; };
// 运行单个 void 子任务，异常写入共享槽；全部完成后投递父协程。
// 参数：task - 子任务（所有权转移）；slot - 异常槽；ctl - 共享完成计数
inline coro::Task<void> run_one_void(coro::Task<void> task, VoidSlot* slot,
                                     JoinCtl* ctl) {
    try { co_await coro::AwaitTask<void>{task}; }
    catch (...) { slot->exc = std::current_exception(); }
    if (ctl->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto parent = ctl->parent;
        ctl->parent = nullptr;
        coro::EventLoop::current().post(parent);
    }
}
}  // namespace detail

// 并发执行两个 void 子任务；任一异常重抛，正常返回 true
// 参数：a, b - 子任务（所有权转移）
inline coro::Task<bool> when_all_void(coro::Task<void> a, coro::Task<void> b) {
    detail::VoidSlot sa, sb;
    auto ctl = std::make_unique<detail::JoinCtl>();
    ctl->remaining.store(2);
    coro::EventLoop& loop = coro::EventLoop::current();
    spawn(detail::run_one_void(std::move(a), &sa, ctl.get()), loop);
    spawn(detail::run_one_void(std::move(b), &sb, ctl.get()), loop);
    co_await detail::JoinAwaiter{ctl.get()};
    if (sa.exc) std::rethrow_exception(sa.exc);
    if (sb.exc) std::rethrow_exception(sb.exc);
    co_return true;
}

}  // namespace net
