// 异步 connection/connectionpool 测试
// 失败路径测试始终执行；集成测试需设置 MYSQL_TEST_HOST/USER/PASSWORD/DB
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <coro/awaiter.h>
#include <coro/event_loop.h>
#include <coro/task.h>

#include "connection.h"
#include "connectionpool.h"
#include "mysql_async_error.h"

// 注：池失败路径测试协程由 test_pool.cpp 的 test_pool_failures() 创建并 post，
// 本文件只用 extern 声明调用入口；而本文件的集成测试（池并发/借用超时）直接
// 使用 connectionpool 类型，故包含 "connectionpool.h"

// ---------- 断言与汇总 ----------
// 注意：g_pass/g_fail 非 static——test_pool.cpp（Task 3 的池测试）用 extern 共享
int g_pass = 0;
int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            ++g_pass;                                                       \
        } else {                                                            \
            ++g_fail;                                                       \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__);             \
        }                                                                   \
    } while (0)

// ---------- 失败路径测试（无服务器即可运行） ----------
// 编排说明（单次 run 统一编排）：全部测试协程（连接 2 个 + 池 3 个）由
// main 的第一次（也是唯一一次）run() 驱动。事件循环 stop 后 stop_flag_ 永真，
// 二次 run() 在队列空时立即返回，挂在定时器上的池协程永远不被驱动——
// 故池测试不再自行 run，全部协程一次 post 后统一 run。
// 停止由统一 stopper 负责：覆盖最长路径（connect_fail 2000ms 与 borrow_fail
// 串行 2×1500ms 并发执行，墙钟 ≈ 3000ms，borrow_fail 瓶颈）→ 4500ms 保证余量。
// 若 127.0.0.1 快速失败（RST）则路径更短，stopper 提前 stop 无碍。

// async_connect 连不存在的服务器 → 抛 MySQLAsyncError 且连接标记为不可用
static coro::Task<void> test_connect_fail(coro::EventLoop& loop) {
    (void)loop;
    connection c;                 // 对象提到 try 外，catch 后检查其内部状态
    bool threw = false;
    try {
        co_await coro::AwaitTask<void>{c.async_connect("127.0.0.1", "root", "", "test", 2000)};
    } catch (const MySQLAsyncError&) {
        threw = true;
    }
    CHECK(threw, "连不存在的服务器抛 MySQLAsyncError");
    CHECK(c.is_valid() == false, "失败后连接标记为不可用（valid_ = false）");
}

// 未连接时 async_query → 抛异常
static coro::Task<void> test_query_not_connected(coro::EventLoop& loop) {
    (void)loop;
    connection c;
    bool threw = false;
    try {
        co_await coro::AwaitTask<MYSQL_RES*>{c.async_query("SELECT 1", 1000)};
    } catch (const MySQLAsyncError&) {
        threw = true;
    }
    CHECK(threw, "未连接时 async_query 抛 MySQLAsyncError");
}

// 所有测试协程执行完毕后统一 stop（覆盖 borrow_fail 串行 2×1500ms 最长路径）
static coro::Task<void> stopper(coro::EventLoop& loop) {
    co_await coro::sleep_for(4500);
    loop.stop();
}

// 池失败路径测试入口（Task 3 在 test_pool.cpp 实现，本文件只声明调用）
extern void test_pool_failures(coro::EventLoop& loop);

// ---------- 集成测试（需设置 MYSQL_TEST_HOST/USER/PASSWORD/DB 环境变量） ----------
// 编排说明（与失败路径相同的"单次 run 统一编排"模式）：三个集成测试协程由
// main 的集成段一次全部 post，由集成 stopper（8000ms）统一停止——测试协程
// 内部一律不发 loop.stop()，避免某个测试提前 stop 截断其他并发测试。
// EventLoop stop 后 stop_flag_ 永真，二次 run() 无法驱动挂起协程，故集成段
// 新建独立 EventLoop（loop2）：全新实例无 stop_flag_ 历史，可正常 run。

// 环境变量读取辅助
struct TestDb {
    std::string host;
    std::string user;
    std::string password;
    std::string db;
    bool available = false;

    static TestDb from_env() {
        TestDb t;
        const char* h = std::getenv("MYSQL_TEST_HOST");
        const char* u = std::getenv("MYSQL_TEST_USER");
        const char* p = std::getenv("MYSQL_TEST_PASSWORD");
        const char* d = std::getenv("MYSQL_TEST_DB");
        if (h && u && d) {
            t.host = h;
            t.user = u;
            t.password = p ? p : "";
            t.db = d;
            t.available = true;
        }
        return t;
    }
};
static TestDb g_db;

// 集成测试完成标志：每个集成测试协程正常结束（含 SKIP 提前返回）时置位。
// loop2.run() 返回后检查：若未全部完成，说明被 8000ms 统一 stopper 截断
// （有协程挂起）——被截断协程的断言不执行不计数，套件仍会 PASS 退出码 0，
// 必须显式记 FAIL，杜绝挂起静默假 PASS。
static constexpr int kIntegrationCount = 3;
static std::atomic<int> g_integration_done{0};
static void mark_integration_done() { g_integration_done.fetch_add(1, std::memory_order_relaxed); }

// 建表 + INSERT + SELECT + UPDATE 全链路（单连接）
// 注意：不发 loop.stop()（统一由集成 stopper 收尾，见编排注释）
static coro::Task<void> test_integration_query_flow(coro::EventLoop& loop) {
    (void)loop;
    connection c;
    try {
        co_await coro::AwaitTask<void>{c.async_connect(g_db.host.c_str(), g_db.user.c_str(),
                                                       g_db.password.c_str(), g_db.db.c_str(), 5000)};
    } catch (const MySQLAsyncError& e) {
        std::printf("SKIP: 连接集成服务器失败: %s\n", e.what());
        mark_integration_done();
        co_return;
    }
    CHECK(c.is_valid(), "async_connect 成功且连接有效");

    // 建表（删旧建新）
    bool create_ok = false;
    try {
        co_await coro::AwaitTask<uint64_t>{c.async_update("DROP TABLE IF EXISTS coro_async_test", 5000)};
        co_await coro::AwaitTask<uint64_t>{c.async_update(
            "CREATE TABLE coro_async_test (id INT PRIMARY KEY, name VARCHAR(64))", 5000)};
        create_ok = true;
    } catch (const MySQLAsyncError& e) {
        std::printf("FAIL create table: %s\n", e.what());
    }
    CHECK(create_ok, "async_update 建表成功");

    if (create_ok) {
        // INSERT
        uint64_t n = 0;
        bool insert_ok = false;
        try {
            n = co_await coro::AwaitTask<uint64_t>{c.async_update(
                "INSERT INTO coro_async_test VALUES (1, 'alice'), (2, 'bob')", 5000)};
            insert_ok = true;
        } catch (const MySQLAsyncError& e) {
            std::printf("FAIL insert: %s\n", e.what());
        }
        CHECK(insert_ok && n == 2, "async_update 插入 2 行，影响行数 = 2");

        // SELECT
        MYSQL_RES* res = nullptr;
        bool select_ok = false;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{c.async_query(
                "SELECT id, name FROM coro_async_test ORDER BY id", 5000)};
            select_ok = true;
        } catch (const MySQLAsyncError& e) {
            std::printf("FAIL select: %s\n", e.what());
        }
        CHECK(select_ok && res != nullptr, "async_query 返回结果集");
        if (res) {
            MYSQL_ROW row;
            int rows = 0;
            while ((row = mysql_fetch_row(res))) {
                CHECK(row[0] && row[1], "行数据完整");
                ++rows;
            }
            CHECK(rows == 2, "查询返回 2 行");
            mysql_free_result(res);
        }

        // UPDATE
        uint64_t u = 0;
        bool update_ok = false;
        try {
            u = co_await coro::AwaitTask<uint64_t>{c.async_update(
                "UPDATE coro_async_test SET name = 'alice2' WHERE id = 1", 5000)};
            update_ok = true;
        } catch (const MySQLAsyncError& e) {
            std::printf("FAIL update: %s\n", e.what());
        }
        CHECK(update_ok && u == 1, "async_update 更新 1 行");

        // 清理现场（DROP TABLE）
        try {
            co_await coro::AwaitTask<uint64_t>{c.async_update("DROP TABLE coro_async_test", 5000)};
        } catch (...) {
        }
    }
    c.close();
    mark_integration_done();
}

// 池并发借用/归还：N 协程各借用→归还若干次，全部成功（计数守恒、无丢失）
// worker 内 try-catch 记录错误：borrow/query 异常不静默（顶层异常信号弱）
static constexpr int kPoolConcurrency = 8;
static constexpr int kPoolIterations = 5;
static std::atomic<int> g_pool_done{0};
static std::atomic<int> g_pool_errors{0};
// 最后一个 worker 完成时恢复集成池主协程（代替简报"最后一个完成时 stop"的编排）
static std::coroutine_handle<> g_pool_done_h;

static coro::Task<void> pool_worker(connectionpool* pool) {
    for (int i = 0; i < kPoolIterations; ++i) {
        connection* c = nullptr;
        try {
            c = co_await coro::AwaitTask<connection*>{pool->async_borrow()};
            // 简单查询验证连接可用
            MYSQL_RES* res = co_await coro::AwaitTask<MYSQL_RES*>{c->async_query("SELECT 1", 5000)};
            if (res) mysql_free_result(res);
        } catch (const MySQLAsyncError&) {
            ++g_pool_errors;
        }
        if (c) pool->release(c);
    }
    // 最后一个 worker 完成时唤醒集成池主协程（它恢复后执行 CHECK）
    if (++g_pool_done == kPoolConcurrency && g_pool_done_h) {
        auto h = g_pool_done_h;
        g_pool_done_h = nullptr;
        h.resume();
    }
}

static coro::Task<void> test_integration_pool(coro::EventLoop& loop) {
    connectionpool::Config cfg;
    cfg.host = g_db.host.c_str();
    cfg.user = g_db.user.c_str();
    cfg.password = g_db.password.c_str();
    cfg.database = g_db.db.c_str();
    cfg.min_size = 2;
    cfg.max_size = 4;
    cfg.borrow_timeout_ms = 3000;
    cfg.connect_timeout_ms = 5000;
    connectionpool pool(cfg);

    g_pool_done = 0;
    g_pool_errors = 0;
    std::vector<coro::Task<void>> workers;
    for (int i = 0; i < kPoolConcurrency; ++i) {
        workers.push_back(pool_worker(&pool));
    }

    // 等待全部 worker 完成的 awaiter：最后一个 worker 完成时 resume 本协程
    // （不发 loop.stop()，统一由集成 stopper 收尾，见编排注释）
    struct PoolAllDoneAwaiter {
        bool await_ready() noexcept { return g_pool_done >= kPoolConcurrency; }
        void await_suspend(std::coroutine_handle<> parent) noexcept { g_pool_done_h = parent; }
        void await_resume() noexcept {}
    };

    for (auto& w : workers) loop.post(w.handle());
    co_await PoolAllDoneAwaiter{};   // 挂起等待全部 worker 完成
    CHECK(g_pool_done == kPoolConcurrency,
          "池并发借用/归还全部完成（min=2 max=4，8 协程 × 5 次）");
    CHECK(g_pool_errors == 0, "并发借用/查询零错误");
    pool.close();
    mark_integration_done();
}

// 借用超时：max=1 借走不还，第二次借用 → 抛 MySQLTimeoutError
// 注意：不发 loop.stop()（统一由集成 stopper 收尾，见编排注释）
static coro::Task<void> test_integration_borrow_timeout(coro::EventLoop& loop) {
    (void)loop;
    connectionpool::Config cfg;
    cfg.host = g_db.host.c_str();
    cfg.user = g_db.user.c_str();
    cfg.password = g_db.password.c_str();
    cfg.database = g_db.db.c_str();
    cfg.min_size = 0;
    cfg.max_size = 1;
    cfg.borrow_timeout_ms = 500;
    cfg.connect_timeout_ms = 5000;
    connectionpool pool(cfg);

    connection* held = nullptr;
    try {
        held = co_await coro::AwaitTask<connection*>{pool.async_borrow()};
    } catch (const MySQLAsyncError& e) {
        // 服务器不可用：打印 SKIP 直接结束，不发 stop，由集成 stopper 收尾
        std::printf("SKIP: 无法从集成服务器借用: %s\n", e.what());
        mark_integration_done();
        co_return;
    }
    CHECK(held != nullptr, "第一个连接借用成功");

    bool timeout_ok = false;
    try {
        auto* c2 = co_await coro::AwaitTask<connection*>{pool.async_borrow()};  // 满池：等待 500ms 后超时
        (void)c2;
    } catch (const MySQLTimeoutError&) {
        timeout_ok = true;
    } catch (const MySQLAsyncError& e) {
        // 兜底：非超时的借用失败也显式打印（不静默吞掉）
        std::printf("FAIL borrow timeout: %s\n", e.what());
    }
    CHECK(timeout_ok, "满池借用超时抛 MySQLTimeoutError");

    // 归还后池恢复正常
    pool.release(held);
    bool reuse_ok = false;
    try {
        auto* c3 = co_await coro::AwaitTask<connection*>{pool.async_borrow()};
        if (c3) {
            reuse_ok = true;
            pool.release(c3);
        }
    } catch (const MySQLAsyncError&) {
    }
    CHECK(reuse_ok, "超时后归还连接可复用");
    pool.close();
    mark_integration_done();
}

// 集成测试统一 stopper：8000ms 覆盖最长路径（borrow_timeout 500ms 超时等待 +
// pool 8×5 次查询 + query_flow 全链路并发，墙钟 ≈ 1-2s，慢服务器留足余量）
static coro::Task<void> integration_stopper(coro::EventLoop& loop) {
    co_await coro::sleep_for(8000);
    loop.stop();
}

// ---------- 入口 ----------
int main() {
    // ---------- 失败路径（现有逻辑不变：单次 run 统一编排） ----------
    coro::EventLoop loop;
    auto t1 = test_connect_fail(loop);
    auto t2 = test_query_not_connected(loop);
    auto s = stopper(loop);
    loop.post(t1.handle());
    loop.post(t2.handle());
    loop.post(s.handle());
    test_pool_failures(loop);   // Task 3 实现的池失败路径测试（内部 post，不 run）
    loop.run();                 // 单次 run 统一驱动全部协程（含挂在定时器上的池协程）

    // ---------- 集成测试（环境变量缺失时打印 SKIP；不记 FAIL） ----------
    g_db = TestDb::from_env();
    if (g_db.available) {
        // 集成段用全新的 EventLoop（loop2）：失败路径的 loop 已 stop，
        // stop_flag_ 永真，在其上二次 run() 无法驱动挂起协程——新实例无此
        // 历史。三个集成测试一次全部 post + 统一集成 stopper + 单次 run：
        // 测试协程内部不发 stop，墙钟由 8000ms stopper 收尾。
        coro::EventLoop loop2;
        g_integration_done = 0;
        auto t6 = test_integration_query_flow(loop2);
        auto t7 = test_integration_pool(loop2);
        auto t8 = test_integration_borrow_timeout(loop2);
        auto s2 = integration_stopper(loop2);
        loop2.post(t6.handle());
        loop2.post(t7.handle());
        loop2.post(t8.handle());
        loop2.post(s2.handle());
        loop2.run();
        // 完成标志检查：3 个集成协程必须全部正常结束（含 SKIP 提前返回）。
        // 若不足 3，说明某协程被 8000ms stopper 截断（挂起未恢复），其断言
        // 未执行也未计数——显式记 FAIL，防止挂起静默假 PASS
        if (g_integration_done.load() < kIntegrationCount) {
            std::printf("FAIL: 集成测试未全部完成（被 stopper 截断挂起协程）done=%d/%d\n",
                        g_integration_done.load(), kIntegrationCount);
            ++g_fail;
        }
    } else {
        std::printf("SKIP: 未设置 MYSQL_TEST_HOST/USER/PASSWORD/DB，跳过集成测试\n");
    }

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
