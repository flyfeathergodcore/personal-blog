#include "tcp/reactor.hpp"

#include <thread>

namespace tcp {

Reactor::Reactor(std::size_t worker_count)
    : loop_(), workers_(&loop_), worker_count_(worker_count) {}

void Reactor::Post(std::coroutine_handle<> task)
{
    loop_.post(task);
}

void Reactor::PostWorker(std::coroutine_handle<> task)
{
    if (auto* worker = workers_.get_next_loop())
        worker->post(task);
    else
        loop_.post(task);
}

void Reactor::PostWorkerAt(std::size_t index, std::coroutine_handle<> task)
{
    if (auto* worker = workers_.get_loop(index))
        worker->post(task);
    else
        loop_.post(task);
}

std::size_t Reactor::WorkerCount() const
{
    if (worker_count_ != 0)
        return worker_count_;
    const std::size_t count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : count;
}

void Reactor::Run(std::function<void()> on_workers_started)
{
    /*
    1. 启动多个线程私有 worker EventLoop，作为 TCP 连接处理线程池
    2. worker 全部就绪后，向每个 worker 投递自己的 accept 协程
    3. 当前线程运行 base loop，只处理信号等控制事件
    */
    workers_.start(WorkerCount());
    if (on_workers_started)
        on_workers_started();
    loop_.run();
    workers_.join();
}

void Reactor::Stop()
{
    workers_.stop();
    loop_.stop();
}

}  // namespace tcp
