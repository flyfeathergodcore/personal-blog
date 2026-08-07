#include "connection.h"
#include "mysql_async_error.h"
#include <coro/awaiter.h>
#include <coro/task.h>
#include <mysql/errmsg.h>   // CR_SERVER_LOST / CR_SERVER_GONE_ERROR（网络类错误码）
#include <coroutine>
#include <chrono>
#include <cstring>
#include <string>

connection::connection() : conn_(nullptr) {}

connection::~connection() {
    close();
}

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

bool connection::is_connected() const {
    return conn_ != nullptr;
}

bool connection::update(const char* query) {
    if (!conn_) {
        return false;
    }
    if (mysql_query(conn_, query)) {
        return false;
    }
    return true;
}

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

bool connection::close() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
        return true;
    }
    return false;
}

// ---------- 异步接口实现 ----------

// 总超时下的剩余毫秒数；<= 0 表示已超时
static int64_t remain_ms(const std::chrono::steady_clock::time_point& deadline) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(deadline - steady_clock::now()).count();
}

coro::Task<void> connection::async_connect(const char* host, const char* user,
                                           const char* password, const char* database,
                                           int64_t timeout_ms) {
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
}

// 发送查询并读响应头、把状态机推进到 COMPLETE；未连接或网络错误抛异常
static coro::Task<void> send_query_st(MYSQL* m, int fd, const char* sql,
                                      const std::chrono::steady_clock::time_point& deadline,
                                      bool* valid_out) {
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
}

coro::Task<MYSQL_RES*> connection::async_query(const char* sql, int64_t timeout_ms) {
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
}

coro::Task<uint64_t> connection::async_update(const char* sql, int64_t timeout_ms) {
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
}