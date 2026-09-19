// 客户端连接池：复用空闲连接 + 并发连接数上限。
// 借用模型：Acquire 取连接（空闲复用 / 未满新建 / 满员 FIFO 等待），用毕 Release 归还。
// 与 WriteLock 同构：等待者 FIFO 队列 + 可取消超时定时器 + 双 resume 防护。
// 单线程事件循环下无锁安全（池与连接同属一个 loop）。
//
// 计数约定：total_ 表示池当前持有的连接总数（idle + 借出），是 max_ 上限的判定依据；
// 新建失败 / 归还时标记失效 / 借出前发现失效 → 各自递减，保证不泄漏计数。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>

#include "coro/task.h"
#include "rpc/rpc_channel.h"

namespace rpc {

class RpcConnectionPool {
public:
    // 构造：设定并发连接上限（同时最多 max 个连接被持有）
    explicit RpcConnectionPool(std::size_t max);
    ~RpcConnectionPool();
    RpcConnectionPool(const RpcConnectionPool&) = delete;             // 禁止拷贝
    RpcConnectionPool& operator=(const RpcConnectionPool&) = delete;  // 禁止拷贝赋值

    // 获取一个连接：空闲优先复用；未达上限则新建（co_await Open）；已满则
    // FIFO 入队等待，Release/丢弃释放名额后回循环顶重试。超时或连接失败返回 nullptr。
    // 参数：host - 主机名/IP；port - 端口；timeout_ms - 新建与等待的总超时（<0 无限）
    coro::Task<std::shared_ptr<RpcChannel>> Acquire(std::string_view host, std::uint16_t port,
                                                    int64_t timeout_ms);

    // 归还连接：healthy=true 时若有等待者直接移交，否则入空闲队列；
    // healthy=false 表示连接已失效（坏连接不回收复用），关闭并从池移除，
    // 释放的名额唤醒队首等待者回循环顶重试。
    void Release(const std::shared_ptr<RpcChannel>& conn, bool healthy = true);

    // 关闭池内全部空闲连接并等待其读循环退出（进程退出/池销毁前调用，
    // 否则 RpcChannel 在读循环协程仍引用 this 时析构 → UB）。
    // 借出的连接由调用方负责 Close。调用后 size() 归零。
    coro::Task<void> CloseAll();

    // 当前池内连接总数（空闲 + 借出）
    std::size_t size() const { return total_; }

private:
    // 等待获取的协程节点。用 shared_ptr 持有：队列与等待协程各持一份引用，
    // 协程醒来时（无论被 Release 移交、被名额唤醒还是定时器到期）节点必然存活，
    // 可安全遍历队列判断自己是否还在——同 WriteLock::Waiter 的防悬垂手法。
    struct Waiter {
        std::coroutine_handle<> h;
        std::size_t timer_id = 0;                       // 0 = 未注册定时器（无限等待）
        std::shared_ptr<RpcChannel> conn;               // Release 移交时填充；空 = 超时/名额释放
    };
    // 挂起入队并注册可取消定时器（同 WriteLock::SuspendAwaiter）
    struct WaitAwaiter {
        RpcConnectionPool* pool;
        Waiter* w;
        int64_t timeout_ms;
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept;
        void await_resume() noexcept {}
    };
    friend struct WaitAwaiter;

    void WakeOne(const std::shared_ptr<Waiter>& w);  // 作废定时器并 resume（双 resume 防护）
    void WakeNext();                                 // 唤醒队首等待者（名额释放/空归还）

    std::size_t max_;        // 并发连接上限
    std::size_t total_ = 0;  // 池当前持有的连接总数
    std::deque<std::shared_ptr<RpcChannel>> idle_;  // 空闲连接（FIFO）
    std::deque<std::shared_ptr<Waiter>> waiters_;   // 等待获取连接的协程（FIFO）
};

}  // namespace rpc
