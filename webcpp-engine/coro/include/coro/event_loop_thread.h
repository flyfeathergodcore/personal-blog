// 单个 worker 线程及其私有 EventLoop。
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "coro/event_loop.h"

namespace coro {

class EventLoopThread {
    /*
    eventloop 线程用途：
    1. 每个线程拥有独立 EventLoop，适合多线程场景
    2. 提供 start/stop/join 接口，方便管理线程生命周期
    3. 提供 loop() 接口，方便在主线程中获取线程私有 EventLoop 指针
    4. 提供线程安全的条件变量 ready_cv_，用于等待线程启动完成
    5. 提供线程安全的互斥锁 mu_，用于保护 loop_ 和 ready_ 状态
    6. 提供线程安全的标志 ready_，用于标记线程是否已启动完成
    7. 提供线程安全的 unique_ptr<EventLoop> loop_，用于管理线程私有 EventLoop
    8. 提供线程安全的 thread_，用于管理线程对象
    9. 提供线程安全的析构函数，确保线程退出后释放资源
    10. 提供线程安全的 start() 方法，创建线程并在线程内部构造 EventLoop，返回值在线程真正可用后才返回
    11. 提供线程安全的 stop() 方法，请求 loop 停止，可从任意线程调用
    12. 提供线程安全的 join() 方法，等待 worker 线程退出
    13. 提供线程安全的 loop() 方法，返回线程私有 EventLoop 指针
    */
public:
    EventLoopThread() = default;
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // 创建线程，并在线程内部构造 EventLoop；返回值在线程真正可用后才返回。
    EventLoop* start();
    // 请求 loop 停止；可从任意线程调用。
    void stop();
    // 等待 worker 线程退出。
    void join();

    EventLoop* loop() const noexcept;

private:
    mutable std::mutex mu_;
    std::condition_variable ready_cv_;
    std::unique_ptr<EventLoop> loop_;
    std::thread thread_;
    bool ready_ = false;
};

}  // namespace coro
