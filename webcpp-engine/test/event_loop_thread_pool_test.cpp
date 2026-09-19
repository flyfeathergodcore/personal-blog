#include "coro/event_loop_thread_pool.h"
#include "coro/task.h"

#include <atomic>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <mutex>

namespace {

coro::Task<void> worker_task(coro::EventLoopThreadPool* pool,
                             std::atomic<int>* completed,
                             std::atomic<int>* stop_once,
                             int expected,
                             std::unordered_set<std::thread::id>* thread_ids,
                             std::mutex* thread_mu) {
    {
        std::lock_guard<std::mutex> lock(*thread_mu);
        thread_ids->insert(std::this_thread::get_id());
    }
    if (completed->fetch_add(1, std::memory_order_relaxed) + 1 == expected &&
        stop_once->exchange(1, std::memory_order_acq_rel) == 0) {
        pool->stop();
    }
    co_return;
}

}  // namespace

int main() {
    constexpr int kWorkers = 4;
    // 超过单个 EventLoop 的 1024 个 MPSC 槽位，覆盖突发满载回退路径。
    constexpr int kTasks = 8192;
    coro::EventLoopThreadPool pool;
    pool.start(kWorkers);

    std::atomic<int> completed{0};
    std::atomic<int> stop_once{0};
    std::unordered_set<std::thread::id> thread_ids;
    std::mutex thread_mu;
    constexpr int kProducers = 4;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&pool, &completed, &stop_once, &thread_ids,
                                &thread_mu, producer] {
            for (int i = producer; i < kTasks; i += kProducers) {
                coro::EventLoop* loop = pool.get_next_loop();
                coro::Task<void> task = worker_task(&pool, &completed, &stop_once,
                                                    kTasks, &thread_ids, &thread_mu);
                loop->post(task.handle());
            }
        });
    }
    for (auto& producer : producers)
        producer.join();
    pool.join();

    if (completed.load(std::memory_order_relaxed) != kTasks) {
        std::cerr << "worker pool did not execute all tasks\n";
        return 1;
    }
    if (thread_ids.size() < 2) {
        std::cerr << "worker pool did not use multiple threads\n";
        return 1;
    }
    return 0;
}
