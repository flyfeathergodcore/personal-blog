// 动态扩容异步连接池实现
#include "connectionpool.h"

#include <utility>

#include <coro/awaiter.h>
#include <coro/event_loop.h>

#include "mysql_async_error.h"

namespace {
// 等待者 id 全局唯一分配。注意：多个连接池实例共享同一 EventLoop 的
// live_timer_ids_ 集合（cancel_timer 按 id 作废定时器），若各池从 1 自增，
// 跨池 id 碰撞会让一个池的 cancel_timer 误作废另一个池的定时器登记，
// 被误伤的借用协程永不恢复（挂死）。故 id 用全局原子计数器分配。
std::atomic<std::size_t> g_next_wait_id{1};
}  // namespace

// ---------- PoolWaitAwaiter ----------

void connectionpool::PoolWaitAwaiter::await_suspend(std::coroutine_handle<> h) {
    h_ = h;
    if (!pool_->register_waiter(this)) {
        // 池已关闭（登记被拒）：立即唤醒，await_resume 检查 closed_ 抛异常。
        // resume 发生在本调用栈（事件循环 resume 栈），不持池锁
        h_.resume();
    }
}

connection* connectionpool::PoolWaitAwaiter::await_resume() {
    // 无锁读 closed_：resume 发生在事件循环栈上，不持池锁；closed_ 为原子
    if (pool_->closed_.load()) {
        throw MySQLAsyncError("connection pool closed", 0);
    }
    if (!granted_) {
        // 超时唤醒：从等待队列移除自己（可能已被 close 先移除，幂等）
        pool_->remove_waiter(id_);
        throw MySQLTimeoutError("borrow timeout");
    }
    return granted_;
}

// ---------- connectionpool ----------

connectionpool::connectionpool(const Config& cfg)
    : host_(cfg.host), user_(cfg.user), password_(cfg.password), database_(cfg.database),
      min_size_(cfg.min_size), max_size_(cfg.max_size),
      borrow_timeout_ms_(cfg.borrow_timeout_ms), connect_timeout_ms_(cfg.connect_timeout_ms) {
    if (min_size_ <= 0) min_size_ = 1;
    if (max_size_ < min_size_) max_size_ = min_size_;
    if (borrow_timeout_ms_ <= 0) borrow_timeout_ms_ = 1;
    if (connect_timeout_ms_ <= 0) connect_timeout_ms_ = 1;
}

connectionpool::~connectionpool() {
    close();
}

bool connectionpool::register_waiter(connectionpool::PoolWaitAwaiter* w) {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_.load()) {
        return false;   // 池已关闭：await_suspend 立即 resume，await_resume 抛异常
    }
    w->id_ = g_next_wait_id.fetch_add(1, std::memory_order_relaxed);
    waiters_.push_back(Waiter{w, w->id_});
    // 借用超时：到期作废需 id 仍存活；被归还唤醒时 cancel_timer
    coro::EventLoop::current().wait_timer_cancelable(borrow_timeout_ms_, w->h_, w->id_);
    return true;
}

void connectionpool::remove_waiter(std::size_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
        if (it->id == id) {
            waiters_.erase(it);
            break;
        }
    }
}

coro::Task<connection*> connectionpool::async_borrow() {
    // 决策 + 空闲领取/占位在同一把锁内一次完成
    bool need_create = false;
    connection* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_.load()) throw MySQLAsyncError("connection pool closed", 0);
        if (!idle_.empty()) {
            c = idle_.front();
            idle_.pop();
        } else if (total_ < max_size_) {
            ++total_;        // 占位：新建失败时回滚
            need_create = true;
        }
        // 满池：need_create = false，c = nullptr → 走等待路径
    }
    if (c) co_return c;

    if (need_create) {
        // 锁外新建：不在锁内做 IO
        auto* nc = new connection();
        try {
            // Task 非 awaitable：co_await 需经 AwaitTask 包装（task.h 的官方等待方式）
            co_await coro::AwaitTask<void>{nc->async_connect(host_.c_str(), user_.c_str(),
                                                             password_.c_str(), database_.c_str(),
                                                             connect_timeout_ms_)};
        } catch (...) {
            delete nc;
            {
                std::lock_guard<std::mutex> lock(mu_);
                --total_;   // 回滚占位
            }
            throw;
        }
        co_return nc;
    }

    // 满池：挂起等待归还 / 超时。
    // 注意：PoolWaitAwaiter w 是借用协程帧内的局部对象，挂起期间帧存活，
    // 池持有的 awaiter 指针在唤醒前始终有效
    PoolWaitAwaiter w;
    w.pool_ = this;
    co_return co_await w;    // 归还唤醒返回连接；超时/关闭抛异常
}

void connectionpool::release(connection* conn) {
    std::coroutine_handle<> to_resume{};
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_.load()) {
            // 池已关闭：直接销毁借用中的连接
            delete conn;
            if (total_ > 0) --total_;
            return;
        }
        if (!waiters_.empty()) {
            // 有等待者：连接交接给队首
            Waiter w = waiters_.front();
            waiters_.pop_front();
            if (coro::EventLoop::current().cancel_timer(w.awaiter->id_)) {
                // 本次取消成功：该等待者由本路径唤醒（授出连接），定时器不会二次 resume
                w.awaiter->granted_ = conn;
                to_resume = w.awaiter->h_;
            } else {
                // 定时器已到期：该等待者由到期路径 resume（抛超时），不能再唤醒。
                // 连接不能丢——交接给下一个等待者（若有），否则放回空闲队列
                if (!waiters_.empty()) {
                    Waiter w2 = waiters_.front();
                    waiters_.pop_front();
                    // 队首等待者的定时器尚未到期（id 存活），取消必成功
                    if (coro::EventLoop::current().cancel_timer(w2.awaiter->id_)) {
                        w2.awaiter->granted_ = conn;
                        to_resume = w2.awaiter->h_;
                    } else {
                        // 防御分支（理论上不可达：waiters_ 登记在前的定时器 id 必存活）：
                        // 放回空闲队列，绝不丢连接
                        idle_.push(conn);
                    }
                } else {
                    idle_.push(conn);
                }
            }
        } else if (conn->is_valid()) {
            idle_.push(conn);
            return;
        } else {
            // 无效连接：销毁并减计数（下次借用自动新建补位）
            delete conn;
            if (total_ > 0) --total_;
            return;
        }
    }
    if (to_resume) to_resume.resume();   // 锁外唤醒等待者
}

void connectionpool::close() {
    std::vector<std::coroutine_handle<>> to_resume;
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_.store(true);
        // 收集等待者并取消其超时定时器（防止到期二次 resume）。
        // cancel_timer 返回 false 的等待者已被到期路径消费（resume 已安排/已发生），
        // 其 await_resume 检查 closed_ 抛异常（remove_waiter 幂等），无需本路径唤醒
        while (!waiters_.empty()) {
            Waiter w = waiters_.front();
            waiters_.pop_front();
            if (coro::EventLoop::current().cancel_timer(w.awaiter->id_)) {
                to_resume.push_back(w.awaiter->h_);
            }
        }
        // 销毁全部空闲连接
        while (!idle_.empty()) {
            delete idle_.front();
            idle_.pop();
            if (total_ > 0) --total_;
        }
    }
    for (auto h : to_resume) h.resume();   // 锁外唤醒：await_resume 检查 closed_ 抛异常
}
