// 事件循环：驱动 IoPoller + 任务队列 + 定时器最小堆 + 统一销毁队列
// 线程模型：run() 可被多个线程同时调用；post/stop/wait_* 任意线程可调
#pragma once

#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "coro/io_poller.h"

namespace coro {

// 协程句柄哈希：GCC 11 的 libstdc++ 未提供 std::hash<std::coroutine_handle<>>（GCC 12+ 才加入），
// 这里自定义按协程帧地址取哈希，跨编译器（GCC 11 / clang libc++）均可用。
struct CoroHandleHash {
    // 按协程帧地址计算哈希
    std::size_t operator()(std::coroutine_handle<> h) const noexcept {
        return std::hash<void*>{}(h.address());
    }
};

class EventLoop {
public:
    // 构造函数：创建唤醒管道与平台 poller，并登记当前线程关联的事件循环
    EventLoop();
    // 析构函数：stop 后统一销毁残留协程帧并关闭唤醒管道
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;  // 禁止拷贝
    EventLoop& operator=(const EventLoop&) = delete;  // 禁止拷贝赋值

    // 当前线程进入事件循环，直到 stop()（可多线程同时调用）
    void run();
    // 便捷版：共 num 个线程跑事件循环（调用者线程 + num-1 个后台线程），
    // 阻塞直到 stop() 后全部线程退出。run(1) 等价于 run()；num <= 0 直接返回。
    // 注意：返回后所有线程已退出，此时可安全析构本循环。
    void run(int num);
    // 通知所有 run() 退出；挂起的 IO/定时器协程全部投递销毁
    void stop();

    // 投递任务到队列，由任意 run() 线程执行
    void post(std::coroutine_handle<> h);
    // 协程完成后的统一销毁入口
    void post_destroy(std::coroutine_handle<> h);
    // ms 毫秒后恢复 h
    void wait_timer(int64_t ms, std::coroutine_handle<> h);
    // 可取消定时器：ms 后恢复 h；若在此之前调 cancel_timer(id)，到期作废（不恢复）。
    // id 由调用方分配（如连接池的等待者 id），恢复/作废后该 id 即失效
    void wait_timer_cancelable(int64_t ms, std::coroutine_handle<> h, std::size_t id);
    // 使 wait_timer_cancelable 注册的 id 到期作废。返回：
    //   true  = 本次取消成功，定时器到期时不会恢复该协程，调用方必须自行恢复（承担 resume 职责）
    //   false = id 未注册，或已被到期路径消费（该协程的恢复已安排/已发生），调用方不得再 resume
    bool cancel_timer(std::size_t id);
    // fd 就绪恢复 h；timeout_ms >= 0 时超时也会恢复，此时 *out_timed_out = true
    void wait_io(int fd, IoPoller::Event ev, std::coroutine_handle<> h,
                 int64_t timeout_ms, bool* out_timed_out);

    // 顶层协程未捕获异常的兜底回调
    void set_error_handler(std::function<void(std::exception_ptr)> h);
    // 顶层协程未捕获异常通知：有 error_handler_ 则调用，否则 stderr 兜底
    void notify_error(std::exception_ptr e);

    // 当前线程关联的事件循环（构造时设置为 thread_local current）
    static EventLoop& current();

private:
    // 定时器条目；io_fd >= 0 表示这是 IO 等待的超时取消条目
    struct TimerEntry {
        int64_t deadline_ms;      // 单调时钟毫秒
        std::coroutine_handle<> handle;
        std::size_t id;           // 与 io_map_ 条目匹配的唯一 id
        bool* out_timed_out;      // 指向协程帧内的标志（IO 超时用）
        int io_fd;                // -1 = 纯定时器，-2 = 可取消定时器（wait_timer_cancelable）
    };
    struct TimerCmp {
        // 小顶堆比较：deadline 更早的条目排前面
        bool operator()(const TimerEntry& a, const TimerEntry& b) const {
            return a.deadline_ms > b.deadline_ms;
        }
    };

    struct IoEntry {
        std::coroutine_handle<> handle;
        std::size_t id;
    };

    // 取当前单调时钟毫秒值（超时计算用）
    int64_t now_ms() const;
    void wake();  // 写唤醒管道

    std::unique_ptr<IoPoller> poller_;
    int wake_fds_[2] = {-1, -1};  // 唤醒管道

    std::mutex mu_;
    // 阻塞在 poller_->wait()（或即将进入）的 run() 线程数。仅在 mu_ 保护下
    // 读写。> 0 时，新的注册/投递必须 wake() 唤醒阻塞线程，否则新事件可能在
    // 当前 wait 超时前被错过；== 0 时循环正忙，无需唤醒——它会在下一轮 wait
    // 前重新计算超时并看到新注册。这是"循环忙时也按门铃"开销的根治点。
    int waiters_ = 0;
    bool stop_flag_ = false;
    std::vector<std::coroutine_handle<>> tasks_;
    std::vector<std::coroutine_handle<>> destroy_tasks_;
    // 存活帧集合（C1）：post/wait_timer/wait_io/post_destroy 登记，destroy 前注销。
    // 协程可能挂起在无注册挂起点（AwaitTask 父协程、TestStop 等），此时
    // tasks_/io_map_/timers_/destroy_tasks_ 都追踪不到它；本集合兜底，
    // ~EventLoop 把剩余帧全部 destroy，杜绝帧泄漏。unordered_set 天然去重
    // （帧可多次登记，如 post 两次）。
    std::unordered_set<std::coroutine_handle<>, CoroHandleHash> live_frames_;
    // 可取消定时器的存活 id 集合：wait_timer_cancelable 登记，cancel_timer/到期消费后移除
    std::unordered_set<std::size_t> live_timer_ids_;
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCmp> timers_;
    std::unordered_map<int, IoEntry> io_map_;
    std::size_t next_id_ = 1;
    std::function<void(std::exception_ptr)> error_handler_;
};

}  // namespace coro
