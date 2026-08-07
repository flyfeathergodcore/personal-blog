// 协程库单元测试：断言宏 + 各组件测试函数（随任务追加）
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "coro/frame_pool.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);         \
        }                                                                       \
    } while (0)

// ---------- FramePool 测试 ----------
static void test_frame_pool() {
    const std::size_t base = coro::FramePool::live_blocks();

    // 1. 小块分配/释放，计数正确
    void* a = coro::FramePool::alloc(64);
    CHECK(a != nullptr, "alloc 64B 成功");
    // C3 回归：池块数据区必须 16 字节对齐（帧可能含 alignas(16)/long double 成员）
    CHECK((reinterpret_cast<std::uintptr_t>(a) % 16) == 0, "池块 16 字节对齐");
    CHECK(coro::FramePool::live_blocks() == base + 1, "分配后计数 +1");
    coro::FramePool::free(a, 64);
    CHECK(coro::FramePool::live_blocks() == base, "释放后计数还原");

    // 2. 边界：恰好等于桶数据区大小
    void* edge = coro::FramePool::alloc(256 - sizeof(void*) * 2);
    CHECK(edge != nullptr, "边界大小分配成功");
    coro::FramePool::free(edge, 256 - sizeof(void*) * 2);

    // 3. 大块回退到 malloc，不计入池计数
    void* big = coro::FramePool::alloc(8192);
    CHECK(big != nullptr, "8KB 分配成功");
    CHECK(coro::FramePool::live_blocks() == base, "大块不计入计数");
    coro::FramePool::free(big, 8192);

    // 4. 压力：10000 次分配/释放
    std::vector<void*> ptrs;
    ptrs.reserve(10000);
    for (int i = 0; i < 10000; ++i) ptrs.push_back(coro::FramePool::alloc(100));
    CHECK(coro::FramePool::live_blocks() == base + 10000, "10000 块全部活跃");
    for (void* p : ptrs) coro::FramePool::free(p, 100);
    CHECK(coro::FramePool::live_blocks() == base, "全部释放后计数还原");
}

// ---------- IoPoller 测试 ----------
#include "coro/io_poller.h"
#include <sys/socket.h>
#include <unistd.h>

static void test_io_poller() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 创建");
    auto poller = coro::create_poller();

    // 1. 初始不可读：50ms 内无事件
    CHECK(poller->add(sv[0], coro::IoPoller::READ), "注册读事件");
    auto evs = poller->wait(50);
    CHECK(evs.empty(), "无数据时超时返回空");

    // 2. 写数据后可读
    CHECK(write(sv[1], "x", 1) == 1, "写入数据");
    evs = poller->wait(500);
    CHECK(evs.size() == 1, "恰好一个就绪事件");
    if (!evs.empty()) {
        CHECK(evs[0].fd == sv[0], "就绪 fd 正确");
        CHECK((evs[0].events & coro::IoPoller::READ) != 0, "就绪事件为可读");
    }

    // 3. modify 换成写事件后，当前不可写（对方没读）→ 超时
    CHECK(poller->modify(sv[0], coro::IoPoller::WRITE), "modify 为写事件");
    evs = poller->wait(50);
    // 注意：unix socket 缓冲区足够大时写事件可能立即就绪，这里不断言为空，
    // 只验证 modify 不报错、wait 不崩溃。

    // 4. remove 后不再有事件
    CHECK(poller->remove(sv[0]), "remove fd");
    evs = poller->wait(50);
    CHECK(evs.empty(), "remove 后无事件");

    close(sv[0]);
    close(sv[1]);
}

// ---------- EventLoop 测试 ----------
#include "coro/event_loop.h"
#include <atomic>

// 测试用最小协程：创建即执行，暴露句柄，终挂起（帧由测试手动 destroy 或交给循环销毁队列）。
// 帧走 FramePool：下方 C1 回归断言用 live_blocks() 追踪回收，若走全局 new 则泄漏无感知。
struct SimpleTask {
    struct promise_type {
        static void* operator new(std::size_t sz) { return coro::FramePool::alloc(sz); }
        static void operator delete(void* p, std::size_t sz) { coro::FramePool::free(p, sz); }
        SimpleTask get_return_object() {
            return SimpleTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
    std::coroutine_handle<> h_{};
};

// 测试用 awaiter：注册定时器
struct TestWaitTimer {
    int64_t ms_;
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) { coro::EventLoop::current().wait_timer(ms_, h); }
    void await_resume() noexcept {}
};
// 测试用 awaiter：post 自己
struct TestPost {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) { coro::EventLoop::current().post(h); }
    void await_resume() noexcept {}
};
// 测试用 awaiter：stop 事件循环
struct TestStop {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) { coro::EventLoop::current().stop(); }
    void await_resume() noexcept {}
};

static std::atomic<int> g_timer_count{0};
static std::atomic<int> g_post_count{0};

// 等 30ms 后计数 +1，再等 50ms 后计数 +2，然后 stop
static SimpleTask timer_task() {
    co_await TestWaitTimer{30};
    g_timer_count += 1;
    co_await TestWaitTimer{50};
    g_timer_count += 2;
    co_await TestStop{};
}
// 被 post 两次后计数 +1
static SimpleTask post_task() {
    co_await TestPost{};
    co_await TestPost{};
    g_post_count++;
    co_await TestStop{};
}
// C1 回归用例：协程挂起在 30ms 定时器上时外部 stop() 已发生。
// 若协程之后还执行到 TestStop 只是无害挂起点；关键是 stop 发生在定时器挂起期间，
// 帧必须被循环完整回收。
static SimpleTask hang_task() {
    co_await TestWaitTimer{30};
    co_await TestStop{};
}

static void test_event_loop() {
    {
        // C1 回归：块前后记录池活跃块数，块结束后必须回落（修复前 timer_task
        // 帧挂起在 TestStop 挂起点，stop() 收集不到 → 每轮泄漏 2 个池块）
        const std::size_t base = coro::FramePool::live_blocks();
        {
            coro::EventLoop loop;
            g_timer_count = 0;
            SimpleTask t = timer_task();
            loop.post(t.h_);
            loop.run();
            CHECK(g_timer_count == 3, "定时器按顺序恢复（30ms +1，50ms +2）");
            // timer_task 帧在 stop 时被循环统一销毁，句柄已失效，不再使用
        }
        CHECK(coro::FramePool::live_blocks() == base, "timer_task 帧全部回收（无泄漏）");
    }
    {
        // C1 回归：post_task 同理（最后挂起在 TestStop）
        const std::size_t base = coro::FramePool::live_blocks();
        {
            coro::EventLoop loop;
            g_post_count = 0;
            SimpleTask t = post_task();
            loop.post(t.h_);
            loop.run();
            CHECK(g_post_count == 1, "post 任务执行完成");
        }
        CHECK(coro::FramePool::live_blocks() == base, "post_task 帧全部回收（无泄漏）");
    }
    {
        // C1 回归：协程挂起在定时器上时外部 stop()，帧全部回收
        const std::size_t base = coro::FramePool::live_blocks();
        {
            coro::EventLoop loop;
            SimpleTask t = hang_task();
            loop.post(t.h_);
            loop.stop();  // 协程尚未 resume/未执行到 TestStop，正挂在定时器上
            loop.run();
        }
        CHECK(coro::FramePool::live_blocks() == base, "定时器挂起帧 stop 后全部回收（无泄漏）");
    }
}

// ---------- Task 测试 ----------
#include "coro/task.h"
#include "coro/awaiter.h"
#include <stdexcept>

static int g_task_result = 0;
static int g_exc_caught = 0;

// 子任务：等 10ms 返回 42
static coro::Task<int> child_task() {
    co_await coro::sleep_for(10);
    co_return 42;
}
// 子任务：抛异常
static coro::Task<int> throw_task() {
    co_await coro::sleep_for(1);
    throw std::runtime_error("boom");
    co_return 0;
}
// 父任务：等子任务，结果 +1 后 stop
static coro::Task<void> parent_task() {
    int v = co_await coro::AwaitTask<int>{child_task()};
    g_task_result = v + 1;
    co_await coro::sleep_for(5);
    coro::EventLoop::current().stop();
}
// 异常传导：父任务捕获子任务异常
static coro::Task<void> exc_parent() {
    try {
        co_await coro::AwaitTask<int>{throw_task()};
    } catch (const std::runtime_error&) {
        g_exc_caught++;
    }
    coro::EventLoop::current().stop();
}
// C1 回归：父协程 co_await 子任务挂起期间被外部 stop()（模拟 stop 时父子链
// 挂在 AwaitTask 上）。子任务挂在 10ms 定时器上被 stop() 收集销毁；
// 父协程帧挂起在 AwaitTask 挂起点（无注册来源），修复前永久泄漏，
// 修复后由 live_frames_ 兜底回收。
static coro::Task<void> leak_check_parent() {
    int v = co_await coro::AwaitTask<int>{child_task()};
    g_task_result = v;
    co_await coro::sleep_for(5);  // 若未被 stop 会继续执行（不会到这里）
}

static void test_task() {
    // 记录本测试开始前的池活跃块数（帧回收验证基准）
    const std::size_t s_pool_base = coro::FramePool::live_blocks();
    {
        coro::EventLoop loop;
        g_task_result = 0;
        coro::Task<void> t = parent_task();
        loop.post(t.handle());
        loop.run();
        CHECK(g_task_result == 43, "父子任务结果传递（42 + 1）");
    }
    {
        coro::EventLoop loop;
        g_exc_caught = 0;
        coro::Task<void> t = exc_parent();
        loop.post(t.handle());
        loop.run();
        CHECK(g_exc_caught == 1, "子任务异常被父任务捕获");
    }
    // C1：父协程挂 AwaitTask 期间 stop()，父子两帧必须全部回收
    const std::size_t base = coro::FramePool::live_blocks();
    {
        coro::EventLoop loop;
        g_task_result = 0;
        coro::Task<void> t = leak_check_parent();
        loop.post(t.handle());
        loop.stop();  // 协程尚未完成：run() 中父挂 AwaitTask、子挂 10ms 定时器时 stop 已生效
        loop.run();
        CHECK(g_task_result == 0, "子任务未完成即 stop，父协程未读取结果");
    }
    CHECK(coro::FramePool::live_blocks() == base, "AwaitTask 挂起父帧全部回收（无泄漏）");
    // 两轮测试结束后：所有协程帧（父/子/孙）均已通过销毁队列回收
    CHECK(coro::FramePool::live_blocks() == s_pool_base, "协程帧全部回收");
}

// ---------- 顶层协程异常测试（C2） ----------
static int g_error_handler_calls = 0;

// 顶层协程：无父协程，直接抛异常（unhandled_exception 存入 promise.exception_）
static coro::Task<void> top_throw_task() {
    throw std::runtime_error("top-level boom");
    co_return;  // 不可达；保证函数体含协程表达式（co_return 也是协程关键字）
}

static void test_top_level_exception() {
    // C2 回归：顶层协程异常不得静默吞没——注册的 error_handler 必须被调用；
    // 且异常帧随销毁队列正常回收
    const std::size_t base = coro::FramePool::live_blocks();
    {
        coro::EventLoop loop;
        g_error_handler_calls = 0;
        // 顶层协程完成/异常后无人再 stop，run() 将永不退出；
        // error_handler 是唯一的收尾点：计数 +1 并 stop 循环
        loop.set_error_handler([&loop](std::exception_ptr) {
            ++g_error_handler_calls;
            loop.stop();
        });
        coro::Task<void> t = top_throw_task();
        loop.post(t.handle());
        loop.run();
        CHECK(g_error_handler_calls == 1, "顶层协程异常触发 error_handler");
    }
    CHECK(coro::FramePool::live_blocks() == base, "异常协程帧全部回收");
}

// ---------- IO 等待测试 ----------
#include <sys/socket.h>
#include <unistd.h>

static int g_io_sock_a = -1, g_io_sock_b = -1;
static int g_io_ready_ok = 0;
static int g_io_timeout_ok = 0;

// 等 fd 可读（数据已提前写入），验证返回 Ready
static coro::Task<void> io_ready_task() {
    auto r = co_await coro::await_readable(g_io_sock_a, 1000);
    g_io_ready_ok = (r == coro::Readiness::Ready);
    coro::EventLoop::current().stop();
}
// 等 fd 可读但对方不写，验证超时返回 Timeout
static coro::Task<void> io_timeout_task() {
    auto r = co_await coro::await_readable(g_io_sock_a, 50);
    g_io_timeout_ok = (r == coro::Readiness::Timeout);
    coro::EventLoop::current().stop();
}

static void test_await_readable() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 创建");
    g_io_sock_a = sv[0];
    g_io_sock_b = sv[1];

    // 1. 就绪路径：先写数据，协程等待后 poll 立即就绪
    {
        coro::EventLoop loop;
        g_io_ready_ok = 0;
        CHECK(write(g_io_sock_b, "x", 1) == 1, "预写数据");
        coro::Task<void> t = io_ready_task();
        loop.post(t.handle());
        loop.run();
        CHECK(g_io_ready_ok == 1, "IO 就绪返回 Ready");
        // 读取数据，恢复 socket 初始状态
        char buf[4];
        CHECK(read(g_io_sock_a, buf, sizeof(buf)) == 1, "读走预写数据");
    }

    // 2. 超时路径：不写数据，50ms 超时
    {
        coro::EventLoop loop;
        g_io_timeout_ok = 0;
        coro::Task<void> t = io_timeout_task();
        loop.post(t.handle());
        loop.run();
        CHECK(g_io_timeout_ok == 1, "IO 超时返回 Timeout");
    }

    close(sv[0]);
    close(sv[1]);
}

// ---------- 多线程测试 ----------
#include <thread>

static constexpr int kWorkers = 100;
static std::atomic<int> g_mt_worker_done{0};

// 工作协程：sleep 若干次后计数；全部完成后 stop
static coro::Task<void> mt_worker() {
    for (int i = 0; i < 5; ++i) co_await coro::sleep_for(1);
    if (++g_mt_worker_done == kWorkers) {
        coro::EventLoop::current().stop();
    }
}

// 从 4 个线程并发 post 启动的协程，各自 sleep 后计数（验证 post 线程安全）
static constexpr int kPostWorkers = 400;
static std::atomic<int> g_mt_post_done{0};
static coro::Task<void> mt_post_worker() {
    co_await coro::sleep_for(1);
    if (++g_mt_post_done == kPostWorkers) {
        coro::EventLoop::current().stop();
    }
}

static void test_multi_thread() {
    // 1. 多线程 run() + 定时器协程压力（3 线程同时 run）
    {
        coro::EventLoop loop;
        g_mt_worker_done = 0;
        std::vector<coro::Task<void>> workers;
        workers.reserve(kWorkers);
        for (int i = 0; i < kWorkers; ++i) workers.push_back(mt_worker());
        for (auto& w : workers) loop.post(w.handle());

        std::thread t1([&loop] { loop.run(); });
        std::thread t2([&loop] { loop.run(); });
        loop.run();  // 主线程也进入
        t1.join();
        t2.join();
        CHECK(g_mt_worker_done == kWorkers, "100 个工作协程全部完成（3 线程 run）");
    }

    // 2. 4 线程并发 post 400 个协程，全部执行无丢失
    {
        coro::EventLoop loop;
        g_mt_post_done = 0;
        std::vector<std::thread> posters;
        for (int t = 0; t < 4; ++t) {
            posters.emplace_back([&loop] {
                for (int i = 0; i < 100; ++i) {
                    coro::Task<void> w = mt_post_worker();
                    loop.post(w.handle());
                }
            });
        }
        loop.run();  // 主线程跑循环；最后一个协程完成时 stop()
        for (auto& t : posters) t.join();
        CHECK(g_mt_post_done == kPostWorkers, "400 个协程全部执行（跨线程 post）");
    }
}

// run(num) 便捷版：调用者 + 后台线程共 num 个跑循环，stop 后全部退出
static void test_run_n() {
    // 1. run(3)：3 个线程（调用者 + 2 后台）跑 100 个协程
    {
        coro::EventLoop loop;
        g_mt_worker_done = 0;
        std::vector<coro::Task<void>> workers;
        workers.reserve(kWorkers);
        for (int i = 0; i < kWorkers; ++i) workers.push_back(mt_worker());
        for (auto& w : workers) loop.post(w.handle());
        loop.run(3);  // 阻塞直到最后一个协程 stop 后全部线程退出
        CHECK(g_mt_worker_done == kWorkers, "run(3)：3 线程跑 100 协程全部完成");
    }
    // 2. run(1) 等价于 run()：调用者单线程跑
    {
        coro::EventLoop loop;
        g_mt_worker_done = 0;
        std::vector<coro::Task<void>> workers;
        workers.reserve(kWorkers);
        for (int i = 0; i < kWorkers; ++i) workers.push_back(mt_worker());
        for (auto& w : workers) loop.post(w.handle());
        loop.run(1);
        CHECK(g_mt_worker_done == kWorkers, "run(1)：单线程跑 100 协程全部完成");
    }
    // 3. run(0)/run(-1)：无操作，立即返回
    {
        coro::EventLoop loop;
        loop.run(0);
        loop.run(-1);
        CHECK(true, "run(0)/run(-1) 直接返回");
    }
}

// ---------- 库扩展测试（await_event / SelfSuspendAwaiter / 可取消定时器） ----------

// await_event 测试辅助协程：等事件或超时，记录结果
static int g_ev_ready = 0;
static int g_ev_timeout = 0;
static coro::Task<void> ev_worker(int fd, coro::IoPoller::Event ev, int64_t ms, bool expect_timeout) {
    auto r = co_await coro::await_event(fd, ev, ms);
    if (expect_timeout) {
        if (r == coro::Readiness::Timeout) ++g_ev_timeout;
    } else {
        if (r == coro::Readiness::Ready) ++g_ev_ready;
    }
    coro::EventLoop::current().stop();  // 每个场景单独跑
}

static void test_await_event() {
    // 1. READ|WRITE 组合：新 socketpair 空写缓冲，WRITE 立即就绪
    {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair ok");
        coro::EventLoop loop;
        g_ev_ready = 0;
        auto t = ev_worker(sv[0], coro::IoPoller::READ | coro::IoPoller::WRITE, 1000, false);
        loop.post(t.handle());
        loop.run();
        CHECK(g_ev_ready == 1, "await_event READ|WRITE 组合就绪");
        close(sv[0]);
        close(sv[1]);
    }
    // 2. 超时路径：只等 READ 且不写数据，50ms 后超时
    {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair ok");
        coro::EventLoop loop;
        g_ev_timeout = 0;
        auto t = ev_worker(sv[0], coro::IoPoller::READ, 50, true);
        loop.post(t.handle());
        loop.run();
        CHECK(g_ev_timeout == 1, "await_event 超时返回 Timeout");
        close(sv[0]);
        close(sv[1]);
    }
}

// SelfSuspendAwaiter：协程挂起后登记 awaiter 指针，主测试线程 resume
static coro::SelfSuspendAwaiter* g_ss_waiter = nullptr;
static int g_ss_resumed = 0;
static coro::Task<void> ss_worker() {
    coro::SelfSuspendAwaiter w;
    g_ss_waiter = &w;            // 挂起前先把 awaiter 地址交给测试线程
    co_await w;                  // 挂起：w.h 在 await_suspend 中被填为本协程句柄
    ++g_ss_resumed;
    coro::EventLoop::current().stop();
}

static void test_self_suspend() {
    coro::EventLoop loop;
    g_ss_waiter = nullptr;
    g_ss_resumed = 0;
    auto t = ss_worker();
    loop.post(t.handle());
    std::thread runner([&loop] { loop.run(); });   // 先进入循环
    // 等协程挂起（g_ss_waiter 已登记且 w.h 已填）
    for (int i = 0; i < 1000 && (!g_ss_waiter || !g_ss_waiter->h); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(g_ss_waiter && g_ss_waiter->h, "协程已挂起并登记句柄");
    if (g_ss_waiter) {
        g_ss_waiter->h.resume();   // 外部唤醒（测试线程）
        loop.stop();               // 协程完成（内部已调 stop）
    }
    runner.join();
    CHECK(g_ss_resumed == 1, "外部 resume 后协程继续执行");
}

// 可取消定时器测试：TestCtAwaiter 在 await_suspend 注册定时器并暴露句柄
struct TestCtAwaiter {
    int64_t ms_;
    std::size_t id_;
    std::coroutine_handle<> h_;

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        h_ = h;
        coro::EventLoop::current().wait_timer_cancelable(ms_, h_, id_);
    }
    void await_resume() noexcept {}
};

static int g_ct_ok = 0;            // 正常到期恢复次数
static int g_ct_cancel = 0;        // 取消后不应恢复
static TestCtAwaiter* g_ct_c_waiter = nullptr;
static coro::Task<void> ct_normal() {
    co_await TestCtAwaiter{30, 100};
    ++g_ct_ok;
    coro::EventLoop::current().stop();
}
static coro::Task<void> ct_cancel() {
    // 30ms 提高为 200ms：取消需在到期前执行，慢机抢占时 30ms 窗口可能被
    // 主线程 sleep 循环错过（低概率 flaky），200ms 保证余量
    TestCtAwaiter w{200, 200};
    g_ct_c_waiter = &w;
    co_await w;
    ++g_ct_cancel;                  // 不应执行到
    coro::EventLoop::current().stop();
}

static void test_cancelable_timer() {
    // 1. 未取消：30ms 后正常恢复
    {
        coro::EventLoop loop;
        g_ct_ok = 0;
        auto t = ct_normal();
        loop.post(t.handle());
        loop.run();
        CHECK(g_ct_ok == 1, "wait_timer_cancelable 到期正常恢复");
    }
    // 2. 已取消：到期作废，协程不被恢复
    {
        coro::EventLoop loop;
        g_ct_cancel = 0;
        g_ct_c_waiter = nullptr;
        auto t = ct_cancel();
        loop.post(t.handle());
        std::thread runner([&loop] { loop.run(); });
        // 等协程挂起登记后取消定时器
        for (int i = 0; i < 1000 && (!g_ct_c_waiter || !g_ct_c_waiter->h_); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(g_ct_c_waiter && g_ct_c_waiter->h_, "协程已挂起并注册可取消定时器");
        if (g_ct_c_waiter) {
            // cancel_timer 返回 true = 本次取消成功（id 存活）；忽略返回值即可，
            // 取消路径语义由事件循环内部保证（到期作废不恢复）
            (void)coro::EventLoop::current().cancel_timer(g_ct_c_waiter->id_);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));  // 跨过 200ms 到期点
            loop.stop();   // 协程仍挂起：stop() 收集并销毁其帧
        }
        runner.join();
        CHECK(g_ct_cancel == 0, "取消后定时器到期作废，协程未被恢复");
    }
}

// ---------- 入口 ----------
int main() {
    test_frame_pool();
    test_io_poller();
    test_event_loop();
    test_task();
    test_top_level_exception();
    test_await_readable();
    test_multi_thread();
    test_run_n();
    test_await_event();
    test_self_suspend();
    test_cancelable_timer();
    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
