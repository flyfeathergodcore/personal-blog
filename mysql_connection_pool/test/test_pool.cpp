// 连接池测试（池失败路径始终执行；集成测试在 test_async.cpp）
#include <cstdio>

#include <coro/awaiter.h>
#include <coro/event_loop.h>
#include <coro/task.h>

#include "connection.h"
#include "connectionpool.h"
#include "mysql_async_error.h"

// 共享 test_async.cpp 的断言计数（宏与 test_async.cpp 一致：成功不打印）
extern int g_pass;
extern int g_fail;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            ++g_pass;                                                       \
        } else {                                                            \
            ++g_fail;                                                       \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__);             \
        }                                                                   \
    } while (0)

// ---------- 池失败路径测试（无服务器即可运行） ----------

// 池新建连接失败（服务器不存在）→ borrow 抛异常，且池仍可用（计数回滚）
static coro::Task<void> test_pool_borrow_fail(coro::EventLoop&) {
    connectionpool::Config cfg;
    cfg.host = "127.0.0.1";   // 无 MySQL 服务
    cfg.user = "root";
    cfg.password = "";
    cfg.database = "test";
    cfg.min_size = 1;
    cfg.max_size = 4;
    cfg.connect_timeout_ms = 1500;
    connectionpool pool(cfg);

    bool threw1 = false;
    try {
        auto* c = co_await coro::AwaitTask<connection*>{pool.async_borrow()};
        (void)c;
    } catch (const MySQLAsyncError&) {
        threw1 = true;
    }
    CHECK(threw1, "池新建连接失败抛 MySQLAsyncError");
    // 计数回滚：再次 borrow 应重新尝试新建（而不是卡死在 total_ == max 或泄漏）
    bool threw2 = false;
    try {
        auto* c = co_await coro::AwaitTask<connection*>{pool.async_borrow()};
        (void)c;
    } catch (const MySQLAsyncError&) {
        threw2 = true;
    }
    CHECK(threw2, "失败后池仍可继续尝试借用（计数已回滚）");
}

// close 后借用抛异常
static coro::Task<void> test_pool_closed(coro::EventLoop&) {
    connectionpool::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.user = "root";
    cfg.password = "";
    cfg.database = "test";
    cfg.min_size = 0;
    cfg.max_size = 2;
    connectionpool pool(cfg);
    pool.close();

    bool threw = false;
    try {
        auto* c = co_await coro::AwaitTask<connection*>{pool.async_borrow()};
        (void)c;
    } catch (const MySQLAsyncError&) {
        threw = true;
    }
    CHECK(threw, "close 后借用抛 MySQLAsyncError");
}

// release 无效连接（未连接的对象直接归还）→ 池销毁它，不影响后续借用流程
static coro::Task<void> test_pool_release_invalid(coro::EventLoop&) {
    connectionpool::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.user = "root";
    cfg.password = "";
    cfg.database = "test";
    cfg.min_size = 0;
    cfg.max_size = 2;
    cfg.connect_timeout_ms = 1500;   // 显式设置：默认 10000ms 会超过 stopper 窗口被截断
    connectionpool pool(cfg);

    auto* c = new connection();   // 从未 connect：is_valid() == false
    pool.release(c);              // 应被销毁（total_ 减计数，不崩溃）
    // 再走一次新建失败路径确认池状态一致（计数未错乱）
    bool threw = false;
    try {
        auto* c2 = co_await coro::AwaitTask<connection*>{pool.async_borrow()};
        (void)c2;
    } catch (const MySQLAsyncError&) {
        threw = true;
    }
    CHECK(threw, "release 无效连接后池状态一致（可继续新建流程）");
    pool.close();
}

// ---------- 池失败路径入口（test_async.cpp 的 main 调用） ----------

// 只 post 三个池测试协程，不调用 run()：由 test_async.cpp 的 main 单次 run
// 统一驱动全部协程（本循环 stop 后 stop_flag_ 永真，二次 run() 在队列空时
// 立即返回，挂在定时器上的池协程永远不被驱动——详见 test_async.cpp 编排注释）。
// 停止由 test_async.cpp 的统一 stopper 负责（覆盖 borrow_fail 最长 ≈3000ms 路径）
void test_pool_failures(coro::EventLoop& loop) {
    auto t3 = test_pool_borrow_fail(loop);
    auto t4 = test_pool_closed(loop);
    auto t5 = test_pool_release_invalid(loop);
    loop.post(t3.handle());
    loop.post(t4.handle());
    loop.post(t5.handle());
}
