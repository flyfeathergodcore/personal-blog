#include "connection.h"
#include "mysql_async_error.h"
#include <coro/awaiter.h>
#include <coro/task.h>
#include <mysql/errmsg.h>   // CR_SERVER_LOST / CR_SERVER_GONE_ERROR（网络类错误码）
#include <coroutine>
#include <chrono>
#include <cstring>
#include <string>

// 总超时下的剩余毫秒数；<= 0 表示已超时
static int64_t remain_ms(const std::chrono::steady_clock::time_point& deadline) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(deadline - steady_clock::now()).count();
}

#ifdef MARIADB_CONNECTOR_C
// ══════════════════════════════════════════════════════════════════════
// mariadb-connector-c 异步分支（Alpine 等无官方 libmysqlclient 的环境）
// ══════════════════════════════════════════════════════════════════════
// mariadb 的异步 API 是 mysql_*_start/cont（MARIADB_ASYNC 宏下暴露），内部是
// ucontext 协程模型（ma_context.c）：*_start 用 my_context_spawn 在独立栈上运行
// 真实调用，需阻塞时 my_context_yield 挂起并返回等待 mask（MYSQL_WAIT_READ/WRITE/
// EXCEPT/TIMEOUT 位或）；应用等 fd 事件就绪后调 *_cont(&ret, m, occurred) 恢复
// 协程（events_occurred = occurred）。官方 async.c 示例即
//   while (status) { poll(fd); status = cont(&ret, m, occurred); }。
// 因此它与 libmysqlclient 的 *_nonblocking 一样是纯事件驱动，无后台线程：
// 挂起期间唯一持有者是本协程栈，超时后直接 mysql_close 安全（无悬空后台线程）。
// 注意两点：
//   * *_cont 唤醒后内部重新尝试 I/O，且只检查 events_occurred 的 TIMEOUT 位决定
//     是否放弃（ma_pvio.c / mariadb_async.c），READ/WRITE 位不参与判断，故等待
//     事件统一按 READ|WRITE 即可，就绪后如实回传就绪位（不含 TIMEOUT）。
//   * 平台要求：nonblock 依赖 x86_64 汇编协程后端；aarch64/musl 无该后端且无
//     可用 ucontext 实现时，mariadb-connector-c 编译为 MY_CONTEXT_DISABLE，
//     mysql_options(MYSQL_OPT_NONBLOCK) 会失败，此时 *_start 因 async_context
//     为 NULL 会段错误。因此 async_connect 必须检查该选项的返回值并给出明确报错。
// 约束：start/cont 必须由同一线程调用——部署模型每 worker 一个事件循环线程、
// 协程始终在同一线程 resume，天然满足。

// 等待 mariadb 非阻塞操作所需事件；返回"已发生事件"位供 *_cont 推进状态机。
// 参数：m - MYSQL 句柄；status - start/cont 返回的等待 mask（本实现统一等 READ|WRITE，
//       精确匹配 mask 非必需）；deadline - 总超时点；what - 异常信息前缀；
//       valid_out - 超时时置 false（可空）
static coro::Task<int> wait_mariadb(MYSQL* m, int status,
                                    const std::chrono::steady_clock::time_point& deadline,
                                    const char* what, bool* valid_out) {
    (void)status;   // 事件由 epoll 统一判定，无需精确匹配 mask 各事件位
    int64_t remain = remain_ms(deadline);
    if (remain <= 0) {
        if (valid_out) *valid_out = false;
        throw MySQLTimeoutError(std::string(what) + " timeout");
    }
    const int fd = mysql_get_socket(m);
    if (fd < 0) {
        // 连接初期 socket 尚未建立（如域名解析阶段）：让出事件循环等 mariadb 更新 mask
        co_await coro::sleep_for(5);
        co_return MYSQL_WAIT_READ | MYSQL_WAIT_WRITE;
    }
    auto r = co_await coro::await_event(fd, coro::IoPoller::READ | coro::IoPoller::WRITE, remain);
    if (r == coro::Readiness::Timeout) {
        if (valid_out) *valid_out = false;
        throw MySQLTimeoutError(std::string(what) + " timeout");
    }
    // 事件就绪：回传就绪事件位。注意不含 MYSQL_WAIT_TIMEOUT——若把 mask 原样回传，
    // 其 TIMEOUT 位会被协程误判为"应放弃"，导致每次等待都被当成超时
    co_return MYSQL_WAIT_READ | MYSQL_WAIT_WRITE;
}
#endif  // MARIADB_CONNECTOR_C

// 构造函数：初始化连接句柄为空
connection::connection() : conn_(nullptr) {}

// 析构函数：关闭并释放数据库连接
connection::~connection() {
    close();
}

// 同步连接数据库；失败返回 false
// 参数：host - 主机名；user - 用户名；password - 密码；database - 数据库名
bool connection::connect(const char* host, const char* user, const char* password, const char* database) {
    conn_ = mysql_init(nullptr);
    if (!conn_) {
        return false;
    }
    if (!mysql_real_connect(conn_, host, user, password, database, 0, nullptr, 0)) {
        mysql_close(conn_);
        conn_ = nullptr;
        return false;
    }
    return true;
}

// 检查当前是否已连接
bool connection::is_connected() const {
    return conn_ != nullptr;
}

// 同步执行更新操作（INSERT/UPDATE/DELETE）；失败返回 false
// 参数：query - SQL 语句
bool connection::update(const char* query) {
    if (!conn_) {
        return false;
    }
    if (mysql_query(conn_, query)) {
        return false;
    }
    return true;
}

// 同步执行查询操作（SELECT），结果存入 *result（调用方负责 mysql_free_result）；失败返回 false
// 参数：query - SQL 语句；result - 输出参数，结果集指针
bool connection::query(const char* query, MYSQL_RES** result) {
    if (!conn_) {
        return false;
    }
    if (mysql_query(conn_, query)) {
        return false;
    }
    *result = mysql_store_result(conn_);
    return true;
}

// 关闭数据库连接；成功关闭返回 true
bool connection::close() {
    if (conn_) {
        // 事件驱动模型下无后台线程：即使异步操作曾超时（mariadb 协程挂起在
        // my_context_yield），mysql_close 也会销毁 async context 并强制关闭 socket，
        // 挂起协程不会被恢复，无悬空访问
        mysql_close(conn_);
        conn_ = nullptr;
        return true;
    }
    return false;
}

// ---------- 异步接口实现 ----------

// 异步连接数据库（非阻塞状态机推进）；总超时后抛 MySQLTimeoutError，失败抛 MySQLAsyncError
// 参数：host - 主机名；user - 用户名；password - 密码；database - 数据库名；timeout_ms - 总超时毫秒数
coro::Task<void> connection::async_connect(const char* host, const char* user,
                                           const char* password, const char* database,
                                           int64_t timeout_ms) {
#ifdef MARIADB_CONNECTOR_C
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    conn_ = mysql_init(nullptr);
    if (!conn_) {
        throw MySQLAsyncError("mysql_init failed", 0);
    }
    // 关键：必须先启用非阻塞模式（创建 async context）。mariadb-connector-c 的
    // nonblock 依赖 x86_64 汇编协程后端；aarch64/musl 等无该后端时此调用会失败
    //（编译为 MY_CONTEXT_DISABLE），若不检查就调用 *_start，会因 async_context
    // 为 NULL 直接段错误
    if (mysql_options(conn_, MYSQL_OPT_NONBLOCK, nullptr) != 0) {
        std::string msg = mysql_error(conn_);
        unsigned int err = mysql_errno(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        if (msg.empty()) {
            msg = "MYSQL_OPT_NONBLOCK 初始化失败（需 x86_64 平台且 mariadb-connector-c 启用 async 支持）";
        }
        throw MySQLAsyncError(msg, err);
    }
    valid_ = false;
    MYSQL* ret = nullptr;
    int status = mysql_real_connect_start(&ret, conn_, host, user, password, database,
                                          0, nullptr, 0);
    while (status) {
        int occurred = co_await wait_mariadb(conn_, status, deadline, "connect", &valid_);
        status = mysql_real_connect_cont(&ret, conn_, occurred);
    }
    if (!ret) {
        // 连接失败：先取错误信息（mysql_close 后句柄失效），再释放，避免 conn_ 悬空
        std::string msg = mysql_error(conn_);
        unsigned int err = mysql_errno(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        throw MySQLAsyncError(msg, err);
    }
    valid_ = true;
#else
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    conn_ = mysql_init(nullptr);
    if (!conn_) {
        throw MySQLAsyncError("mysql_init failed", 0);
    }
    net_async_status st;
    while (true) {
        st = mysql_real_connect_nonblocking(conn_, host, user, password, database,
                                            0, nullptr, 0);
        if (st == NET_ASYNC_COMPLETE) break;
        if (st == NET_ASYNC_ERROR) {
            // 先取错误信息（mysql_close 后句柄失效），再释放，避免 conn_ 悬空
            std::string msg = mysql_error(conn_);
            unsigned int err = mysql_errno(conn_);
            mysql_close(conn_);
            conn_ = nullptr;
            throw MySQLAsyncError(msg, err);
        }
        // NET_ASYNC_NOT_READY：等 fd 可读/可写后推进状态机
        int64_t remain = remain_ms(deadline);
        if (remain <= 0) {
            mysql_close(conn_);
            conn_ = nullptr;
            throw MySQLTimeoutError("connect timeout");
        }
        auto r = co_await coro::await_event(conn_->net.fd,
                                            coro::IoPoller::READ | coro::IoPoller::WRITE, remain);
        if (r == coro::Readiness::Timeout) {
            mysql_close(conn_);
            conn_ = nullptr;
            throw MySQLTimeoutError("connect timeout");
        }
    }
    valid_ = true;
#endif
}

// 发送查询并读响应头、把状态机推进到 COMPLETE；未连接或网络错误抛异常
static coro::Task<void> send_query_st(MYSQL* m, int fd, const char* sql,
                                      const std::chrono::steady_clock::time_point& deadline,
                                      bool* valid_out) {
#ifdef MARIADB_CONNECTOR_C
    (void)fd;   // mariadb 分支经 mysql_get_socket 取 fd，无需外部传入
    int rc = 0;
    int status = mysql_real_query_start(&rc, m, sql, static_cast<unsigned long>(strlen(sql)));
    while (status) {
        int occurred = co_await wait_mariadb(m, status, deadline, "query", valid_out);
        status = mysql_real_query_cont(&rc, m, occurred);
    }
    if (rc != 0) {
        // 区分错误类型：仅网络类错误（连接断开/服务器消失）使连接失效；
        // 应用层错误（如 1146 表不存在、1064 语法错误）连接仍可用
        const unsigned int err = mysql_errno(m);
        if (err == CR_SERVER_LOST || err == CR_SERVER_GONE_ERROR) *valid_out = false;
        throw MySQLAsyncError(mysql_error(m), err);
    }
#else
    net_async_status st;
    while (true) {
        // 必须用 mysql_real_query_nonblocking：内部完成"发送 + 读响应头"两步，
        // 状态机达到 COMPLETE 时响应头（OK/ERR 包）已消费。若用
        // mysql_send_query_nonblocking（只发送不读响应头），后续无法继续推进
        // 状态机（阻塞版 mysql_read_query_result 在非阻塞句柄上返回 0 失败，
        // errno=0 空错误），任何真实服务器上 async_query/async_update 必失败
        st = mysql_real_query_nonblocking(m, sql, static_cast<unsigned long>(strlen(sql)));
        if (st == NET_ASYNC_COMPLETE) co_return;
        if (st == NET_ASYNC_ERROR) {
            // 区分错误类型：仅网络类错误（连接断开/服务器消失）使连接失效；
            // 应用层错误（如 1146 表不存在、1064 语法错误）连接仍可用
            const unsigned int err = mysql_errno(m);
            if (err == CR_SERVER_LOST || err == CR_SERVER_GONE_ERROR) *valid_out = false;
            throw MySQLAsyncError(mysql_error(m), err);
        }
        int64_t remain = remain_ms(deadline);
        if (remain <= 0) {
            *valid_out = false;
            throw MySQLTimeoutError("query timeout");
        }
        auto r = co_await coro::await_event(fd, coro::IoPoller::READ | coro::IoPoller::WRITE, remain);
        if (r == coro::Readiness::Timeout) {
            *valid_out = false;
            throw MySQLTimeoutError("query timeout");
        }
    }
#endif
}

// 异步查询（SELECT）：发送查询并读取结果集；总超时后抛 MySQLTimeoutError，网络类错误置连接无效
// 参数：sql - SQL 语句；timeout_ms - 总超时毫秒数
// 返回：结果集指针（调用方负责 mysql_free_result）
coro::Task<MYSQL_RES*> connection::async_query(const char* sql, int64_t timeout_ms) {
#ifdef MARIADB_CONNECTOR_C
    using namespace std::chrono;
    if (!conn_) {
        throw MySQLAsyncError("not connected", 0);
    }
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);

    // 阶段 1：发送 + 读响应头（send_query_st 内由 mysql_real_query_start/cont 完成）
    co_await coro::AwaitTask<void>{send_query_st(conn_, 0, sql, deadline, &valid_)};

    // 阶段 2：取结果集。store_result 完成后 MYSQL_RES 已完整拉入内存，
    // 调用方在事件循环线程同步 mysql_fetch_row 为纯内存操作，安全。
    MYSQL_RES* res = nullptr;
    int status = mysql_store_result_start(&res, conn_);
    while (status) {
        int occurred = co_await wait_mariadb(conn_, status, deadline, "query", &valid_);
        status = mysql_store_result_cont(&res, conn_, occurred);
    }
    // store_result 返回 NULL 且带错误码：应用层错误（如 1146）连接仍可用，
    // 不置 valid_，只抛异常（errno == 0 时无错误，属正常 NULL 结果）
    if (res == nullptr && mysql_errno(conn_) != 0) {
        throw MySQLAsyncError(mysql_error(conn_), mysql_errno(conn_));
    }
    co_return res;
#else
    using namespace std::chrono;
    if (!conn_) {
        throw MySQLAsyncError("not connected", 0);
    }
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    const int fd = conn_->net.fd;

    // 阶段 1：发送 + 读响应头（send_query_st 内由 mysql_real_query_nonblocking
    // 一次完成，COMPLETE 时响应头已消费；错误已在其中抛异常）
    co_await coro::AwaitTask<void>{send_query_st(conn_, fd, sql, deadline, &valid_)};

    // 阶段 2：取结果集
    MYSQL_RES* res = nullptr;
    net_async_status st;
    while (true) {
        st = mysql_store_result_nonblocking(conn_, &res);
        if (st == NET_ASYNC_COMPLETE) break;
        if (st == NET_ASYNC_ERROR) {
            valid_ = false;
            throw MySQLAsyncError(mysql_error(conn_), mysql_errno(conn_));
        }
        int64_t remain = remain_ms(deadline);
        if (remain <= 0) {
            valid_ = false;
            throw MySQLTimeoutError("query timeout");
        }
        auto r = co_await coro::await_event(fd, coro::IoPoller::READ, remain);
        if (r == coro::Readiness::Timeout) {
            valid_ = false;
            throw MySQLTimeoutError("query timeout");
        }
    }
    // store_result 返回 NULL 且带错误码：应用层错误（如 1146）连接仍可用，
    // 不置 valid_，只抛异常（errno == 0 时无错误，属正常 NULL 结果）
    if (res == nullptr && mysql_errno(conn_) != 0) {
        throw MySQLAsyncError(mysql_error(conn_), mysql_errno(conn_));
    }
    co_return res;
#endif
}

// 异步更新（INSERT/UPDATE/DELETE）：发送查询并返回影响行数；总超时后抛 MySQLTimeoutError
// 参数：sql - SQL 语句；timeout_ms - 总超时毫秒数
// 返回：受影响行数
coro::Task<uint64_t> connection::async_update(const char* sql, int64_t timeout_ms) {
#ifdef MARIADB_CONNECTOR_C
    using namespace std::chrono;
    if (!conn_) {
        throw MySQLAsyncError("not connected", 0);
    }
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    co_await coro::AwaitTask<void>{send_query_st(conn_, 0, sql, deadline, &valid_)};
    // send_query_st COMPLETE 时 OK 包已消费（响应头读完），
    // mysql_affected_rows 立即可用；错误（网络/应用层）已在 send_query_st 抛异常
    co_return mysql_affected_rows(conn_);
#else
    using namespace std::chrono;
    if (!conn_) {
        throw MySQLAsyncError("not connected", 0);
    }
    const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    const int fd = conn_->net.fd;

    co_await coro::AwaitTask<void>{send_query_st(conn_, fd, sql, deadline, &valid_)};

    // send_query_st COMPLETE 时 OK 包已消费（响应头读完），
    // mysql_affected_rows 立即可用；错误（网络/应用层）已在 send_query_st 抛异常
    co_return mysql_affected_rows(conn_);
#endif
}
