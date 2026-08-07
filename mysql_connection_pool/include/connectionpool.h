// 动态扩容异步连接池：空闲→直接给；未到上限→新建；满→协程挂起等待归还
// 前置条件：池析构前调用 close()（或确保无挂起的 async_borrow），且事件循环仍存活
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <queue>
#include <string>

#include <coro/task.h>

#include "connection.h"

class connectionpool {
public:
    struct Config {
        const char* host;
        const char* user;
        const char* password;
        const char* database;
        int min_size = 4;                   // 初始连接数
        int max_size = 16;                  // 上限
        int64_t borrow_timeout_ms = 5000;   // 借用超时（满池等待时生效）
        int64_t connect_timeout_ms = 10000; // 新建连接超时
    };

    explicit connectionpool(const Config& cfg);
    ~connectionpool();   // 调用 close()

    connectionpool(const connectionpool&) = delete;
    connectionpool& operator=(const connectionpool&) = delete;

    // 借连接：空闲直接给；未到上限新建；满池挂起等待，直到有连接归还或 borrow_timeout 超时
    coro::Task<connection*> async_borrow();
    // 还连接：有等待者直接交接（唤醒队首）；无等待者放回空闲队列；无效连接销毁补位
    void release(connection* conn);
    // 关闭：唤醒全部等待者（它们抛 MySQLAsyncError 退出），销毁全部连接
    void close();

private:
    struct PoolWaitAwaiter;   // 前置声明：Waiter 持有其指针

    // 等待者登记：直接持有 awaiter 指针（归还/关闭时改它的 granted_ 并 resume 它的 h_）
    // 注意：PoolWaitAwaiter 是借用协程帧内的局部对象（co_await w 挂起期间帧存活），
    // 池持有的指针在等待者被唤醒（或池 close 清空 waiters_）前始终有效
    struct Waiter {
        PoolWaitAwaiter* awaiter;
        std::size_t id;
    };

    // 池内等待者专用 awaiter（co_await 时登记到池，归还/超时唤醒）
    struct PoolWaitAwaiter {
        connectionpool* pool_;
        std::coroutine_handle<> h_;
        std::size_t id_ = 0;
        connection* granted_ = nullptr;   // 归还方交接的连接；null = 尚未获得

        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h);
        connection* await_resume();   // 被归还唤醒返回连接；超时/关闭抛异常
    };

    // 以下均在 mu_ 保护下操作
    bool register_waiter(PoolWaitAwaiter* w);   // 登记 + 注册借用超时定时器；池已关闭返回 false
    void remove_waiter(std::size_t id);         // 等待者自我清理（超时唤醒后）

    std::mutex mu_;
    std::string host_, user_, password_, database_;
    int min_size_, max_size_;
    int64_t borrow_timeout_ms_, connect_timeout_ms_;

    std::queue<connection*> idle_;
    int total_ = 0;                       // 空闲 + 借用中
    std::deque<Waiter> waiters_;          // FIFO 等待者
    // 注意：等待者 id 由 connectionpool.cpp 的全局原子计数器分配（跨池唯一，
    // 多个池共享同一 EventLoop 的 live_timer_ids_，池内自增会跨池碰撞）
    // 原子标志：await_resume 在无锁路径读取（被唤醒时不持池锁），
    // 其他读写点在 mu_ 内——atomic 保证该无锁读无数据竞争
    std::atomic<bool> closed_{false};
};
