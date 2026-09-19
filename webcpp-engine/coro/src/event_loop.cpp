// 事件循环实现
#include "coro/event_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

namespace coro
{

    namespace
    {
        thread_local EventLoop *g_current_loop = nullptr;
    } // namespace

    EventLoop::EventLoop()
    {
        /*
        1. Linux 使用 eventfd；macOS/BSD 使用 pipe，均用于唤醒阻塞在 poll 中的线程
        2. poller_ 的创建与 wake fd 注册延后到 run()，保证由 owner 线程完成
        3. 将当前线程关联的事件循环设置为g_current_loop
        */
        for (std::size_t i = 0; i < kPendingPoolSize; ++i)
            pending_pool_[i].sequence.store(i, std::memory_order_relaxed);

#ifdef __linux__
        const int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd >= 0)
        {
            wake_fds_[0] = event_fd;
            wake_fds_[1] = event_fd;
        }
#else
        if (pipe(wake_fds_) == 0)
        {
            int fl = fcntl(wake_fds_[0], F_GETFL, 0);
            fcntl(wake_fds_[0], F_SETFL, fl | O_NONBLOCK);
            fcntl(wake_fds_[0], F_SETFD, FD_CLOEXEC);
            fl = fcntl(wake_fds_[1], F_GETFL, 0);
            fcntl(wake_fds_[1], F_SETFL, fl | O_NONBLOCK);
            fcntl(wake_fds_[1], F_SETFD, FD_CLOEXEC);
        }
#endif
        g_current_loop = this;
    }

    EventLoop::~EventLoop()
    {
        /*
        1. 调用stop()停止事件循环
        2. 收集所有待销毁的协程句柄，包括tasks_、destroy_tasks_和live_frames_中的句柄
        3. 销毁所有收集到的协程句柄，确保协程资源被释放，关闭唤醒管道
        4. 如果当前线程关联的事件循环是this，则将g_current_loop置为nullptr
        */
        stop();
        std::unordered_set<std::coroutine_handle<>, CoroHandleHash> to_destroy;
        {
            std::lock_guard<std::mutex> lock(mu_);
            // 未运行的 loop 没有 owner；析构阶段已无并发访问，可由当前线程完成回收。
            if (owner_thread_ == std::thread::id{})
                owner_thread_ = std::this_thread::get_id();
            if (owner_thread_ == std::this_thread::get_id())
                process_stop_request_locked();
            drain_pending_tasks_locked();
            to_destroy.insert(tasks_.begin(), tasks_.end());
            to_destroy.insert(destroy_tasks_.begin(), destroy_tasks_.end());
            to_destroy.insert(live_frames_.begin(), live_frames_.end());
            tasks_.clear();
            destroy_tasks_.clear();
            live_frames_.clear();
        }
        for (auto h : to_destroy)
            h.destroy();
        if (g_current_loop == this)
            g_current_loop = nullptr;
        if (wake_fds_[0] >= 0)
            close(wake_fds_[0]);
        if (wake_fds_[1] >= 0 && wake_fds_[1] != wake_fds_[0])
            close(wake_fds_[1]);
    }

    EventLoop &EventLoop::current()
    {
        // 获取当前线程关联的事件循环；若当前线程未关联 loop（例如从未构造），惰性创建
        if (!g_current_loop)
            new EventLoop();
        return *g_current_loop;
    }

    int64_t EventLoop::now_ms() const
    {
        // 获取当前单调时钟毫秒值（用于超时计算）
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // 写平台唤醒 fd。只保留一个未消费通知，避免高频 post 反复写 pipe/eventfd。
    void EventLoop::wake()
    {
        /*
        1. 检查唤醒写端wake_fds_[1]是否有效，或者是否已经有未消费的唤醒通知（wake_pending_为true）
            - 如果满足条件，直接返回，不进行写操作
            - 否则，将wake_pending_设置为true，表示有一个未消费的唤醒通知
        2. 循环尝试写入唤醒通知：
            - Linux使用eventfd，写入一个uint64_t值1
            - macOS/BSD使用pipe，写入一个unsigned char值1
            - 如果写入成功，或者写入失败但errno为EAGAIN（表示缓冲区已满），则返回
            - 如果写入失败且errno为EINTR（表示被中断），继续尝试写入
            - 如果写入失败且errno为其他值，表示写入失败，重置wake_pending_为false，并返回
        */
        if (wake_fds_[1] < 0 || wake_pending_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        for (;;)
        {
#ifdef __linux__
            const std::uint64_t one = 1;
            const ssize_t written = ::write(wake_fds_[1], &one, sizeof(one));
#else
            const unsigned char one = 1;
            const ssize_t written = ::write(wake_fds_[1], &one, sizeof(one));
#endif
            if (written > 0 || (written < 0 && errno == EAGAIN))
                return;
            if (written < 0 && errno == EINTR)
                continue;
            wake_pending_.store(false, std::memory_order_release);
            return;
        }
    }

    void EventLoop::assert_owner_thread() const
    {
        if (owner_thread_ != std::this_thread::get_id())
            throw std::logic_error("EventLoop state must be accessed by its run() thread");
    }

    void EventLoop::post(std::coroutine_handle<> h)
    {
        // 任务进入目标 EventLoop 的 MPSC 队列；调用方不需要获取 mu_。
        if (!enqueue_task(h))
        {
            // 突发流量填满 1024 个无锁槽位时，回退到受锁保护的队列，保证不丢任务。
            std::lock_guard<std::mutex> lock(mu_);
            tasks_.push_back(h);
            live_frames_.insert(h);
        }
        // 无条件尝试唤醒，避免生产者与 poller 进入阻塞之间的竞态。
        // wake_pending_ 会合并重复通知，不会为每个任务都写一次 eventfd/pipe。
        wake();
    }

    bool EventLoop::enqueue_task(std::coroutine_handle<> h)
    {
        std::size_t pos = pending_enqueue_pos_.load(std::memory_order_relaxed);
        do
        {
            PendingTask &slot = pending_pool_[pos & kPendingPoolMask];
            const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::intptr_t>(sequence) -
                              static_cast<std::intptr_t>(pos);
            if (diff == 0)
            {
                if (pending_enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    slot.handle = h;
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                return false;
            }
            else
            {
                pos = pending_enqueue_pos_.load(std::memory_order_relaxed);
            }
        } while (true);
    }

    void EventLoop::drain_pending_tasks_locked()
    {
        // 只有 owner 线程消费；序号确保槽位循环复用时不会发生 ABA。
        for (;;)
        {
            const std::size_t pos = pending_dequeue_pos_;
            PendingTask &slot = pending_pool_[pos & kPendingPoolMask];
            const std::size_t sequence = slot.sequence.load(std::memory_order_acquire);
            if (sequence != pos + 1)
                return;

            tasks_.push_back(slot.handle);
            live_frames_.insert(slot.handle);
            slot.sequence.store(pos + kPendingPoolSize, std::memory_order_release);
            ++pending_dequeue_pos_;
        }
    }

    void EventLoop::post_destroy(std::coroutine_handle<> h)
    {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            destroy_tasks_.push_back(h);
            live_frames_.insert(h);
            need_wake = waiters_.load(std::memory_order_relaxed) > 0;
        }
        if (need_wake)
            wake();
    }

    void EventLoop::wait_timer(int64_t ms, std::coroutine_handle<> h)
    {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            timers_.push({now_ms() + ms, h, 0, nullptr, -1});
            live_frames_.insert(h); // C1
            need_wake = waiters_.load(std::memory_order_relaxed) > 0;
        }
        if (need_wake)
            wake();
    }

    void EventLoop::wait_timer_cancelable(int64_t ms, std::coroutine_handle<> h, std::size_t id)
    {
        bool need_wake = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            // io_fd = -2 标记可取消定时器（-1 为普通定时器，>= 0 为 IO 等待超时条目）
            timers_.push({now_ms() + ms, h, id, nullptr, -2});
            live_timer_ids_.insert(id);
            live_frames_.insert(h);
            need_wake = waiters_.load(std::memory_order_relaxed) > 0;
        }
        if (need_wake)
            wake();
    }

    bool EventLoop::cancel_timer(std::size_t id)
    {
        std::lock_guard<std::mutex> lock(mu_);
        // true = 本次取消成功（定时器到期将作废，调用方必须自行 resume 协程）；
        // false = id 未注册或已被到期路径消费（resume 已安排/已发生），调用方不得再 resume
        return live_timer_ids_.erase(id) != 0;
    }

    // 注册 fd 事件等待：就绪或超时（timeout_ms >= 0）时恢复 h；循环空闲时唤醒阻塞线程
    // 参数：fd - 文件描述符；ev - 事件组合；h - 协程句柄；timeout_ms - 超时毫秒数（-1 = 无限）；out_timed_out - 超时标志输出
    void EventLoop::wait_io(int fd, IoPoller::Event ev, std::coroutine_handle<> h,
                            int64_t timeout_ms, bool *out_timed_out)
    {
        /*
        1. await_event() 只能从被本 EventLoop 恢复的协程调用
        2. io_map_ 与 poller_ 由 owner 线程独占
        3. 生命周期集合和定时器仍由 mu_ 保护；跨线程 post()/stop() 使用队列和 wake fd
        */
        assert_owner_thread();
        *out_timed_out = false;
        const std::size_t id = next_id_++;
        io_map_[fd] = IoEntry{h, id};
        poller_->add(fd, ev);
        {
            // 仅生命周期集合和定时器与控制路径共享；io_map_ / poller_ 不在此锁内。
            std::lock_guard<std::mutex> lock(mu_);
            live_frames_.insert(h);
            if (timeout_ms >= 0)
                timers_.push({now_ms() + timeout_ms, h, id, out_timed_out, fd});
        }
    }

    void EventLoop::set_error_handler(std::function<void(std::exception_ptr)> h)
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_handler_ = std::move(h);
    }

    void EventLoop::notify_error(std::exception_ptr e)
    {

        std::function<void(std::exception_ptr)> handler;
        {
            std::lock_guard<std::mutex> lock(mu_);
            handler = error_handler_;
        }
        if (handler)
        {
            handler(e);
        }
        else
        {
            std::fprintf(stderr, "[coro] 未处理的协程异常\n");
        }
    }

    void EventLoop::stop()
    {
        /*
        1. stop() 可从任意线程调用，但不直接触碰 io_map_ 或 poller_
        2. 设置停止请求后写 wake fd，唤醒 owner 线程
        3. owner 线程在 run() 中调用 process_stop_request_locked() 完成资源回收
        */
        stop_requested_.store(true, std::memory_order_release);
        wake();
    }

    void EventLoop::process_stop_request_locked()
    {
        assert_owner_thread();
        if (!stop_requested_.load(std::memory_order_acquire) || stop_flag_)
            return;

        std::vector<std::coroutine_handle<>> to_destroy;
        drain_pending_tasks_locked();
        stop_flag_ = true;
        while (!timers_.empty())
        {
            TimerEntry t = timers_.top();
            timers_.pop();
            if (t.io_fd >= 0)
            {
                auto it = io_map_.find(t.io_fd);
                if (it != io_map_.end() && it->second.id == t.id)
                {
                    to_destroy.push_back(it->second.handle);
                    io_map_.erase(it);
                }
                continue;
            }
            if (t.io_fd == -2)
            {
                if (live_timer_ids_.count(t.id) != 0)
                {
                    live_timer_ids_.erase(t.id);
                    to_destroy.push_back(t.handle);
                }
                continue;
            }
            to_destroy.push_back(t.handle); // 纯定时器（io_fd == -1）
        }
        for (auto &kv : io_map_)
            to_destroy.push_back(kv.second.handle);
        io_map_.clear();
        for (auto h : to_destroy)
            live_frames_.insert(h);
        destroy_tasks_.insert(destroy_tasks_.end(), to_destroy.begin(), to_destroy.end());
    }

    void EventLoop::run()
    {
        /*
        1. 将当前线程关联的事件循环设置为g_current_loop
        2. 进入事件循环，处理任务队列、销毁队列、IO事件和定时器
        3. 在每次循环中，先处理任务队列中的协程句柄，调用resume()恢复它们的执行
        4. 然后处理销毁队列中的协程句柄，调用destroy()销毁它们的资源
        5. 接着计算下一个定时器的超时时间，并调用poller_->wait()等待IO事件或定时器到期
        6. 当有IO事件就绪时，从io_map_中取出对应的协程句柄，调用resume()恢复它们的执行
        7. 当有定时器到期时，从timers_中取出对应的协程句柄，调用resume()恢复它们的执行
        8. 如果stop_flag_为true，并且任务队列和销毁队列都为空，则退出事件循环
        9. 该函数确保在事件循环中，所有相关的协程句柄都被正确恢复和销毁，避免资源泄漏
        10. 该函数在多线程环境下使用了互斥锁mu_来保护共享数据结构，确保线程安全
        */
        g_current_loop = this;
        owner_thread_ = std::this_thread::get_id();
        // poller 的创建与 wake fd 注册都在 owner 线程完成。
        if (!poller_)
        {
            poller_ = create_poller();
            if (wake_fds_[0] >= 0)
                poller_->add_level(wake_fds_[0], IoPoller::READ);
        }
        std::vector<std::coroutine_handle<>> ready;
        while (true)
        {
            ready.clear();
            {
                std::lock_guard<std::mutex> lock(mu_);
                drain_pending_tasks_locked();
                process_stop_request_locked();
                if (stop_flag_ && tasks_.empty() && destroy_tasks_.empty())
                    return;
                ready.swap(tasks_);
            }
            for (auto h : ready)
            {
                h.resume();
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                ready.clear();
                ready.swap(destroy_tasks_);
            }
            for (auto h : ready)
            {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    live_frames_.erase(h);
                }
                h.destroy();
            }
            int timeout = -1;
            {
                std::lock_guard<std::mutex> lock(mu_);
                process_stop_request_locked();
                if (!timers_.empty())
                {
                    int64_t d = timers_.top().deadline_ms - now_ms();
                    timeout = d > 0 ? static_cast<int>(d) : 0;
                }
                if (stop_flag_ && tasks_.empty() && destroy_tasks_.empty())
                    return;
                if (timeout < 0 || timeout > 50)
                    timeout = 50;
                waiters_.fetch_add(1, std::memory_order_relaxed); // 本线程即将阻塞于 poller_->wait
            }
            auto events = poller_->wait(timeout);
            {
                std::lock_guard<std::mutex> lock(mu_);
                waiters_.fetch_sub(1, std::memory_order_relaxed);
            }

            ready.clear();
            {
                std::lock_guard<std::mutex> lock(mu_);
                // IO 就绪：从 io_map_ 取出并恢复
                for (auto &e : events)
                {
                    if (e.fd == wake_fds_[0])
                    {
                        // 清空唤醒状态。eventfd 的一次 read 会复位计数器；pipe 则排空。
#ifdef __linux__
                        std::uint64_t value;
                        while (::read(wake_fds_[0], &value, sizeof(value)) == sizeof(value))
                        {
                        }
#else
                        char buf[64];
                        ssize_t r;
                        do
                        {
                            r = read(wake_fds_[0], buf, sizeof(buf));
                        } while (r > 0);
#endif
                        wake_pending_.store(false, std::memory_order_release);
                        continue;
                    }
                    auto it = io_map_.find(e.fd);
                    if (it != io_map_.end())
                    {
                        ready.push_back(it->second.handle);
                        io_map_.erase(it);
                        // ET 模式：不摘除注册。fd 保持注册，消费方读到 EAGAIN 后
                        // 重新 add 挂起；状态翻转触发一次，无 level-triggered 忙转。
                    }
                }
                // 到期定时器
                const int64_t now = now_ms();
                while (!timers_.empty() && timers_.top().deadline_ms <= now)
                {
                    TimerEntry t = timers_.top();
                    timers_.pop();
                    if (t.io_fd >= 0)
                    {
                        // IO 超时取消：若该等待仍存活则标记超时并恢复
                        auto it = io_map_.find(t.io_fd);
                        if (it != io_map_.end() && it->second.id == t.id)
                        {
                            *t.out_timed_out = true;
                            ready.push_back(it->second.handle);
                            io_map_.erase(it);
                            // ET 模式：超时也不摘除注册，与就绪路径一致。
                        }
                        // 若等待已被 IO 就绪消费，定时器作废（忽略）
                    }
                    else if (t.io_fd == -2)
                    {
                        // 可取消定时器：id 仍存活（未被 cancel_timer）才恢复；
                        // 已取消（如等待者被归还唤醒）则作废，防止二次 resume
                        if (live_timer_ids_.count(t.id) != 0)
                        {
                            live_timer_ids_.erase(t.id);
                            ready.push_back(t.handle);
                        }
                    }
                    else
                    {
                        ready.push_back(t.handle);
                    }
                }
            }
            for (auto h : ready)
            {
                h.resume();
            }
        }
    }

    void EventLoop::run(int num)
    {
        /*
        1. EventLoop 只允许当前线程执行自己的 poller，不隐式创建共享 loop 线程
        2. num == 1 时进入本 loop；num != 1 时拒绝旧的共享 loop 用法
        3. 多 worker 服务必须通过 EventLoopThreadPool 创建多个独立 EventLoop
        */
        if (num != 1)
            throw std::invalid_argument("EventLoop::run(num) no longer shares one loop; use EventLoopThreadPool");
        run();
    }

} // namespace coro
