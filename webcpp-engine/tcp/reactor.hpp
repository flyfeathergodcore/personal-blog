#pragma once

#include "coro/event_loop.h"
#include "coro/event_loop_thread_pool.h"

#include <cstddef>
#include <coroutine>
#include <functional>

namespace tcp {

// TCP service reactor. The wrapped coroutine event loop uses epoll with
// edge-triggered readiness on Linux and kqueue on macOS/BSD.
class Reactor {
public:
    explicit Reactor(std::size_t worker_count = 0);

    void Post(std::coroutine_handle<> task);
    void PostWorker(std::coroutine_handle<> task);
    void PostWorkerAt(std::size_t index, std::coroutine_handle<> task);
    // worker 启动后执行回调，适合把 accept 协程投递到各自的 worker loop。
    void Run(std::function<void()> on_workers_started = {});
    void Stop();

    std::size_t WorkerCount() const;

    coro::EventLoop& Native() { return loop_; }

private:
    coro::EventLoop loop_;
    coro::EventLoopThreadPool workers_;
    std::size_t worker_count_ = 0;
};

}  // namespace tcp
