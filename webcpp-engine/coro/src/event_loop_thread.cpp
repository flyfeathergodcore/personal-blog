// 单个 EventLoop worker 线程实现
#include "coro/event_loop_thread.h"

namespace coro {

EventLoopThread::~EventLoopThread() {
    /*
    1. 请求线程内 EventLoop 停止并唤醒 poller
    2. 等待线程退出，确保 EventLoop 不再被访问
    3. 线程退出后由 unique_ptr 释放 loop 及其协程资源
    */
    stop();
    join();
}

EventLoop* EventLoopThread::start() {
    /*
    1. 如果线程已经启动，直接返回已有的 EventLoop 指针
    2. 否则，创建一个新的线程并在其中构造 EventLoop
    3. 使用条件变量等待线程内 EventLoop 构造完成后再返回指针
    */
    std::unique_lock<std::mutex> lock(mu_);
    if (thread_.joinable())
        return loop_.get();

    // 陷入线程函数，构造 EventLoop 并运行事件循环
    thread_ = std::thread([this] {
        auto loop = std::make_unique<EventLoop>();
        {
            std::lock_guard<std::mutex> ready_lock(mu_);
            loop_ = std::move(loop);
            ready_ = true;
        }
        // 构造完成，通知主线程可以获取 loop_ 指针
        ready_cv_.notify_one();
        loop_->run();
    });
    ready_cv_.wait(lock, [this] { return ready_; });
    return loop_.get();
}

void EventLoopThread::stop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (loop_)
        loop_->stop();
}

void EventLoopThread::join() {
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
        thread_.join();
}

EventLoop* EventLoopThread::loop() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return loop_.get();
}

}  // namespace coro
