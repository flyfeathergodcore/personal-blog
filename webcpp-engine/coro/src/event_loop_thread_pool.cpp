// Muduo 风格 EventLoop 线程池实现
#include "coro/event_loop_thread_pool.h"

#include <stdexcept>
#include <thread>

namespace coro {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* base_loop)
    : base_loop_(base_loop) {}

EventLoopThreadPool::~EventLoopThreadPool() {
    /*
    1. 停止每个 worker 的私有 EventLoop
    2. 等待所有线程退出
    3. 清空 loop 指针，避免析构后继续分发连接
    */
    stop();
    join();
    loops_.clear();
    threads_.clear();
}

void EventLoopThreadPool::start(std::size_t worker_count) {
    /*
    1.如果已经启动过线程池，抛出逻辑错误异常
    2.如果worker_count为0，使用硬件并发数作为线程数
    3.为每个worker创建一个EventLoopThread对象，并启动线程
    */
    if (!threads_.empty())
        throw std::logic_error("EventLoopThreadPool::start called twice");

    if (worker_count == 0)
        worker_count = std::thread::hardware_concurrency();
    if (worker_count == 0)
        worker_count = 1;
    threads_.reserve(worker_count);
    loops_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        auto thread = std::make_unique<EventLoopThread>();
        EventLoop* loop = thread->start();
        loops_.push_back(loop);
        threads_.push_back(std::move(thread));
    }
}

void EventLoopThreadPool::stop() {
    /*
    1. 向全部 worker 发送停止请求
    2. 每个 EventLoop 通过自己的 eventfd/pipe 唤醒阻塞中的 poller
    3. worker 清空待销毁队列后退出
    */
    for (auto& thread : threads_)
        thread->stop();
}

void EventLoopThreadPool::join() {
    for (auto& thread : threads_)
        thread->join();
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    if (loops_.empty())
        return base_loop_;
    const std::size_t index = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    return loops_[index];
}

EventLoop* EventLoopThreadPool::get_loop(std::size_t index) {
    if (loops_.empty())
        return base_loop_;
    return loops_[index % loops_.size()];
}

EventLoop* EventLoopThreadPool::get_loop_for_hash(std::size_t hash) {
    if (loops_.empty())
        return base_loop_;
    return loops_[hash % loops_.size()];
}

}  // namespace coro
