// Muduo 风格 EventLoop 线程池：一个线程对应一个独立 EventLoop。
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "coro/event_loop.h"
#include "coro/event_loop_thread.h"

namespace coro {

class EventLoopThreadPool {
    /*
    eventloop 线程池用途：
    1. 多线程场景请使用 EventLoopThreadPool，让每个 worker �拥有独立 EventLoop
    2. 提供轮询和 hash 两种 worker 分配策略，适合不同的连接分片策略
    3. 提供 start/stop/join 接口，方便管理线程池生命周期
    4. 提供 base_loop() 接口，方便在主线程中处理 accept 和信号等监听器事件
    */
public:
    explicit EventLoopThreadPool(EventLoop* base_loop = nullptr);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void start(std::size_t worker_count);
    void stop();
    void join();

    EventLoop* get_next_loop();
    // 返回指定编号的 worker；仅在线程池 start() 完成后使用。
    EventLoop* get_loop(std::size_t index);
    EventLoop* get_loop_for_hash(std::size_t hash);

    std::size_t size() const noexcept { return loops_.size(); }
    EventLoop* base_loop() const noexcept { return base_loop_; }

private:
    EventLoop* base_loop_ = nullptr;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
    std::atomic<std::size_t> next_{0};
};

}  // namespace coro
