// 事件循环：驱动 IoPoller + 任务队列 + 定时器最小堆 + 统一销毁队列
// 线程模型：一个 EventLoop 对应一个线程；post/stop 可从其他线程调用。
// 多核场景请使用 EventLoopThreadPool，让每个 worker 拥有独立 EventLoop。
#pragma once

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "coro/io_poller.h"

namespace coro
{

    struct CoroHandleHash
    {
        /*
        设置协程句柄的哈希函数，用于在 unordered_set 中存储协程句柄。
        该哈希函数使用协程句柄的地址作为哈希值，确保每个协程句柄在集合中唯一标识。
        */
        std::size_t operator()(std::coroutine_handle<> h) const noexcept
        {
            return std::hash<void *>{}(h.address());
        }
    };

    class EventLoop
    {
        /*
        eventloop 用途：
        1. 事件循环：驱动 IoPoller + 任务队列 + 定时器最小堆 + 统一销毁队列
        2. 线程模型：一个 EventLoop 对应一个线程；post/stop 可从其他线程调用
        3. 多核场景请使用 EventLoopThreadPool，让每个 worker 拥有独立 EventLoop
        4. 提供协程挂起等待 fd 事件的接口，支持超时处理
        5. 提供协程挂起等待定时器的接口，支持可取消定时器
        6. 提供错误处理接口，允许注册异常处理函数

        主要接口：
        - run()：启动事件循环，处理任务队列和定时器
        - stop()：停止事件循环，清理资源
        - post(h)：将协程句柄 h 无锁投递到 MPSC 队列，等待 owner 线程执行
        - post_destroy(h)：将协程句柄 h 添加到销毁队列，等待销毁
        - wait_timer(ms, h)：挂起协程 h，等待 ms 毫秒后恢复
        - wait_timer_cancelable(ms, h, id)：挂起协程 h，等待 ms 毫秒后恢复，可通过 id 取消
        - cancel_timer(id)：取消指定 id 的定时器，如果定时器已到期或不存在，返回 false，否则返回 true
        - wait_io(fd, ev, h, timeout_ms, out_timed_out)：挂起协程 h，等待 fd 上的事件 ev 就绪或超时，out_timed_out 标记是否超时
        - set_error_handler(h)：注册异常处理函数 h，当协程抛出异常时调用
        - notify_error(e)：通知异常 e，调用注册的异常处理函数，如果未注册则打印错误信息
        - current()：获取当前线程关联的事件循环，如果未创建则惰性创建
        */
    public:
        EventLoop();
        ~EventLoop();

        EventLoop(const EventLoop &) = delete;            // 禁止拷贝
        EventLoop &operator=(const EventLoop &) = delete; // 禁止拷贝赋值

        void run();
        void run(int num);
        void stop();
        void post(std::coroutine_handle<> h);
        void post_destroy(std::coroutine_handle<> h);
        void wait_timer(int64_t ms, std::coroutine_handle<> h);
        void wait_timer_cancelable(int64_t ms, std::coroutine_handle<> h, std::size_t id);
        bool cancel_timer(std::size_t id);
        void wait_io(int fd, IoPoller::Event ev, std::coroutine_handle<> h,
                     int64_t timeout_ms, bool *out_timed_out);

        void set_error_handler(std::function<void(std::exception_ptr)> h);
        void notify_error(std::exception_ptr e);

        static EventLoop &current();

    private:
        struct TimerEntry
        {
            int64_t deadline_ms;
            std::coroutine_handle<> handle;
            std::size_t id;
            bool *out_timed_out;
            int io_fd;
        };
        struct TimerCmp
        {
            bool operator()(const TimerEntry &a, const TimerEntry &b) const
            {
                return a.deadline_ms > b.deadline_ms;
            }
        };

        struct IoEntry
        {
            std::coroutine_handle<> handle;
            std::size_t id;
        };

        // 有界 MPSC 槽位：sequence 区分循环复用前后的同一物理槽位。
        struct PendingTask
        {
            std::atomic<std::size_t> sequence{0};
            std::coroutine_handle<> handle;
        };

        static constexpr std::size_t kPendingPoolSize = 1024;
        static constexpr std::size_t kPendingPoolMask = kPendingPoolSize - 1;
        static_assert((kPendingPoolSize & kPendingPoolMask) == 0,
                      "MPSC queue capacity must be a power of two");

        int64_t now_ms() const;
        void wake();
        void assert_owner_thread() const;
        bool enqueue_task(std::coroutine_handle<> h);
        void drain_pending_tasks_locked();
        void process_stop_request_locked();

        std::unique_ptr<IoPoller> poller_;
        int wake_fds_[2] = {-1, -1};
        std::atomic<bool> wake_pending_{false};
        std::array<PendingTask, kPendingPoolSize> pending_pool_{};
        std::atomic<std::size_t> pending_enqueue_pos_{0};
        std::size_t pending_dequeue_pos_ = 0;

        std::mutex mu_;
        std::atomic<int> waiters_{0};
        std::atomic<bool> stop_requested_{false};
        bool stop_flag_ = false;
        std::thread::id owner_thread_{};
        std::vector<std::coroutine_handle<>> tasks_;
        std::vector<std::coroutine_handle<>> destroy_tasks_;
        std::unordered_set<std::coroutine_handle<>, CoroHandleHash> live_frames_;
        std::unordered_set<std::size_t> live_timer_ids_;
        std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCmp> timers_;
        std::unordered_map<int, IoEntry> io_map_;
        std::size_t next_id_ = 1;
        std::function<void(std::exception_ptr)> error_handler_;
    };

} // namespace coro
