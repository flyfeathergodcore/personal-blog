// 事件循环实现
#include "coro/event_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace coro {

namespace {
thread_local EventLoop* g_current_loop = nullptr;
}  // namespace

EventLoop::EventLoop() {
    // 唤醒管道（非阻塞），post/stop 时写入以唤醒 poll 阻塞
    if (pipe(wake_fds_) == 0) {
        int fl = fcntl(wake_fds_[0], F_GETFL, 0);
        fcntl(wake_fds_[0], F_SETFL, fl | O_NONBLOCK);
        fl = fcntl(wake_fds_[1], F_GETFL, 0);
        fcntl(wake_fds_[1], F_SETFL, fl | O_NONBLOCK);
    }
    poller_ = create_poller();
    if (wake_fds_[0] >= 0) poller_->add(wake_fds_[0], IoPoller::READ);
    g_current_loop = this;
}

EventLoop::~EventLoop() {
    stop();
    // C1：收集所有残留帧统一销毁。
    //  - tasks_：已 post 但从未被 run() 取走（run() 从未调用或退出前残留）
    //  - destroy_tasks_：已 post_destroy 但从未被 drain（run() 未运行）
    //  - live_frames_：挂起在无注册挂起点（AwaitTask 父协程、TestStop 等）的帧，
    //    stop() 收集不到、run() 退出也不销毁——本集合兜底回收。
    // 帧可能同时出现在多处（如 post 登记的帧），先并入集合去重，避免双重 destroy。
    std::unordered_set<std::coroutine_handle<>, CoroHandleHash> to_destroy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        to_destroy.insert(tasks_.begin(), tasks_.end());
        to_destroy.insert(destroy_tasks_.begin(), destroy_tasks_.end());
        to_destroy.insert(live_frames_.begin(), live_frames_.end());
        tasks_.clear();
        destroy_tasks_.clear();
        live_frames_.clear();
    }
    for (auto h : to_destroy) h.destroy();
    if (g_current_loop == this) g_current_loop = nullptr;
    if (wake_fds_[0] >= 0) close(wake_fds_[0]);
    if (wake_fds_[1] >= 0) close(wake_fds_[1]);
}

EventLoop& EventLoop::current() {
    // 若当前线程未关联 loop（例如从未构造），惰性创建
    if (!g_current_loop) new EventLoop();
    return *g_current_loop;
}

int64_t EventLoop::now_ms() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void EventLoop::wake() {
    if (wake_fds_[1] >= 0) {
        // 尽量写满唤醒管道：多线程 run() 下 kqueue 的 kevent 等待可能只唤醒
        // 部分线程，且某线程消费（读空）管道后其他线程的 kevent 不再就绪。
        // 写满管道使 level-triggered 事件持续存在，所有正在等待的 run() 线程
        // 都能返回；管道满（EAGAIN）说明已写满，直接放弃。
        char buf[64];
        std::fill_n(buf, sizeof(buf), 1);
        while (write(wake_fds_[1], buf, sizeof(buf)) > 0) {
        }
    }
}

void EventLoop::post(std::coroutine_handle<> h) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push_back(h);
        live_frames_.insert(h);  // C1：登记存活帧（帧挂起于任意挂起点后仍被追踪）
    }
    wake();
}

void EventLoop::post_destroy(std::coroutine_handle<> h) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        destroy_tasks_.push_back(h);
        // C1：final_suspend 的帧同样登记——若 run() 已退出或从未被调用
        //（destroy_tasks_ 无人 drain），~EventLoop 仍能凭 live_frames_ 回收
        live_frames_.insert(h);
    }
    wake();
}

void EventLoop::wait_timer(int64_t ms, std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(mu_);
    timers_.push({now_ms() + ms, h, 0, nullptr, -1});
    live_frames_.insert(h);  // C1
    wake();
}

void EventLoop::wait_timer_cancelable(int64_t ms, std::coroutine_handle<> h, std::size_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // io_fd = -2 标记可取消定时器（-1 为普通定时器，>= 0 为 IO 等待超时条目）
    timers_.push({now_ms() + ms, h, id, nullptr, -2});
    live_timer_ids_.insert(id);
    live_frames_.insert(h);
    wake();
}

bool EventLoop::cancel_timer(std::size_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // true = 本次取消成功（定时器到期将作废，调用方必须自行 resume 协程）；
    // false = id 未注册或已被到期路径消费（resume 已安排/已发生），调用方不得再 resume
    return live_timer_ids_.erase(id) != 0;
}

void EventLoop::wait_io(int fd, IoPoller::Event ev, std::coroutine_handle<> h,
                        int64_t timeout_ms, bool* out_timed_out) {
    std::lock_guard<std::mutex> lock(mu_);
    *out_timed_out = false;
    const std::size_t id = next_id_++;
    io_map_[fd] = IoEntry{h, id};
    live_frames_.insert(h);  // C1
    poller_->add(fd, ev);
    if (timeout_ms >= 0) {
        timers_.push({now_ms() + timeout_ms, h, id, out_timed_out, fd});
    }
    wake();
}

void EventLoop::set_error_handler(std::function<void(std::exception_ptr)> h) {
    std::lock_guard<std::mutex> lock(mu_);
    error_handler_ = std::move(h);
}

void EventLoop::notify_error(std::exception_ptr e) {
    // C2：顶层协程未捕获异常兜底。有回调则调用，否则 stderr 提示（绝不静默吞没）。
    // 回调在锁外执行，避免回调内部重入本循环（post/stop 等）时持锁。
    std::function<void(std::exception_ptr)> handler;
    {
        std::lock_guard<std::mutex> lock(mu_);
        handler = error_handler_;
    }
    if (handler) {
        handler(e);
    } else {
        std::fprintf(stderr, "[coro] 未处理的协程异常\n");
    }
}

void EventLoop::stop() {
    std::vector<std::coroutine_handle<>> to_destroy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_flag_ = true;
        // 先扫定时器：纯定时器直接收；IO 等待的超时条目仅当该等待仍存活
        // （io_map_ 中 id 匹配）时收。若等待已被 IO 就绪恢复（run() 中已从
        // io_map_ 取出），残留的超时条目指向已完成帧，收集会导致该帧被
        // 双重销毁（UB）——必须跳过。
        while (!timers_.empty()) {
            TimerEntry t = timers_.top();
            timers_.pop();
            if (t.io_fd >= 0) {
                auto it = io_map_.find(t.io_fd);
                if (it != io_map_.end() && it->second.id == t.id) {
                    // 存活等待：帧与超时条目同一帧，只收一次并同步移除
                    // io_map_ 条目，避免下面再次收集
                    to_destroy.push_back(it->second.handle);
                    io_map_.erase(it);
                }
                continue;
            }
            if (t.io_fd == -2) {
                // 可取消定时器：镜像 run() 的做法——仅当 id 仍存活
                // （未被 cancel_timer）时收集销毁；已取消条目的帧已被调用方
                // 销毁，再次收集指向已销毁帧会双重销毁（UB，实测 live_blocks 变负）
                if (live_timer_ids_.count(t.id) != 0) {
                    live_timer_ids_.erase(t.id);
                    to_destroy.push_back(t.handle);
                }
                continue;
            }
            to_destroy.push_back(t.handle);   // 纯定时器（io_fd == -1）
        }
        // 剩余 IO 等待（未注册超时条目或未被定时器收集的）统一销毁
        for (auto& kv : io_map_) to_destroy.push_back(kv.second.handle);
        io_map_.clear();
        // C1：stop() 收集的帧登记存活集合（收集来源 wait_timer/wait_io 已登记过，
        // 此处为防御性登记，防未来注册来源变化）；随后随 destroy_tasks_ 统一销毁
        for (auto h : to_destroy) live_frames_.insert(h);
        destroy_tasks_.insert(destroy_tasks_.end(), to_destroy.begin(), to_destroy.end());
    }
    wake();
}

void EventLoop::run() {
    // 本线程进入循环后，current() 必须指向本循环。
    // 否则协程在本线程被 resume 时调用 sleep_for/stop 等，会经 thread_local
    // current() 惰性创建"错误的新循环"（current() 对未关联线程会 new EventLoop），
    // 导致定时器/stop 全部落到错误循环，本循环 run() 永久阻塞。
    // 注意：current() 惰性创建语义保留，但构造线程之外只有 run() 会设置它，
    // 线程退出时 thread_local 指针随线程销毁，无悬垂风险。
    g_current_loop = this;
    std::vector<std::coroutine_handle<>> ready;
    while (true) {
        ready.clear();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_flag_ && tasks_.empty() && destroy_tasks_.empty()) return;
            ready.swap(tasks_);
        }
        // 任务与销毁在锁外执行，避免 resume 重入持锁
        for (auto h : ready) {
            h.resume();
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            ready.clear();
            ready.swap(destroy_tasks_);
        }
        for (auto h : ready) {
            {
                // C1：destroy 前注销存活集合，避免 ~EventLoop 对已销毁帧二次 destroy
                std::lock_guard<std::mutex> lock(mu_);
                live_frames_.erase(h);
            }
            h.destroy();
        }

        // 计算 poll 超时：距最近定时器到期的时间
        int timeout = -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!timers_.empty()) {
                int64_t d = timers_.top().deadline_ms - now_ms();
                timeout = d > 0 ? static_cast<int>(d) : 0;
            }
        }
        // stop() 若在上一轮 resume 期间被调用（协程内 TestStop），其 wake 字节
        // 已被本轮消费；此时若队列已空，wait(-1) 将无唤醒源永久阻塞，
        // 循环顶部的退出检查永远轮不到。故在 wait 前再检查一次退出条件。
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_flag_ && tasks_.empty() && destroy_tasks_.empty()) return;
        }
        // 多线程兜底：wait 最长 50ms。多线程 run() 下即使唤醒字节被其他线程
        // 消费（kqueue 只唤醒部分 kevent 等待者），本线程也会定期醒来
        // 重新检查退出条件，杜绝 stop 后部分 run() 线程永久阻塞。
        if (timeout < 0 || timeout > 50) timeout = 50;
        auto events = poller_->wait(timeout);

        ready.clear();
        {
            std::lock_guard<std::mutex> lock(mu_);
            // IO 就绪：从 io_map_ 取出并恢复
            for (auto& e : events) {
                if (e.fd == wake_fds_[0]) {
                    // 唤醒管道：清空并忽略
                    char buf[64];
                    ssize_t r;
                    do { r = read(wake_fds_[0], buf, sizeof(buf)); } while (r > 0);
                    continue;
                }
                auto it = io_map_.find(e.fd);
                if (it != io_map_.end()) {
                    ready.push_back(it->second.handle);
                    io_map_.erase(it);
                    // 消费后移除注册，防止 level-triggered 忙转
                    //（数据未清空时 poll 会立即再次返回该 fd；后续 wait_io 会重新 add）
                    poller_->remove(e.fd);
                }
            }
            // 到期定时器
            const int64_t now = now_ms();
            while (!timers_.empty() && timers_.top().deadline_ms <= now) {
                TimerEntry t = timers_.top();
                timers_.pop();
                if (t.io_fd >= 0) {
                    // IO 超时取消：若该等待仍存活则标记超时并恢复
                    auto it = io_map_.find(t.io_fd);
                    if (it != io_map_.end() && it->second.id == t.id) {
                        *t.out_timed_out = true;
                        ready.push_back(it->second.handle);
                        io_map_.erase(it);
                        // 超时取消同样移除注册，防止 level-triggered 忙转
                        poller_->remove(t.io_fd);
                    }
                    // 若等待已被 IO 就绪消费，定时器作废（忽略）
                } else if (t.io_fd == -2) {
                    // 可取消定时器：id 仍存活（未被 cancel_timer）才恢复；
                    // 已取消（如等待者被归还唤醒）则作废，防止二次 resume
                    if (live_timer_ids_.count(t.id) != 0) {
                        live_timer_ids_.erase(t.id);
                        ready.push_back(t.handle);
                    }
                } else {
                    ready.push_back(t.handle);
                }
            }
        }
        for (auto h : ready) {
            h.resume();
        }
    }
}

void EventLoop::run(int num) {
    if (num <= 0) return;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(num - 1));
    try {
        // 启动 num-1 个后台线程，全部跑同一个循环（共享本实例的队列/表）
        for (int i = 1; i < num; ++i) {
            threads.emplace_back([this] { run(); });
        }
    } catch (...) {
        // 后台线程创建失败：先回收已创建的线程（stop 后它们会退出），再向上抛
        stop();
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        throw;
    }
    // 调用者线程也进入循环；stop() 后所有 run() 返回，join 收尾
    run();
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

}  // namespace coro
