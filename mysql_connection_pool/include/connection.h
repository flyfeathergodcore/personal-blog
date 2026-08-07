#pragma once
#include <mysql/mysql.h>

/*
简单的数据库连接类，封装了mysql的连接、查询、更新等操作
connect - 连接数据库
is_connected - 检查是否连接成功
update - 执行更新操作（INSERT、UPDATE、DELETE）
query - 执行查询操作（SELECT），返回结果集
close - 关闭数据库连接
 */

// coro::Task 是模板类，这里只做前向声明；完整定义在 coro/task.h（由 src/connection.cpp 引入）
namespace coro {
template <typename T>
class Task;
}

class connection
{
public:
    // 构造函数：初始化连接句柄为空
    connection();
    // 析构函数：关闭并释放数据库连接
    ~connection();
    // 同步连接数据库；失败返回 false
    // 参数：host - 主机名；user - 用户名；password - 密码；database - 数据库名
    bool connect(const char* host, const char* user, const char* password, const char* database);
    // 检查当前是否已连接
    bool is_connected() const;
    // 同步执行更新操作（INSERT/UPDATE/DELETE）；失败返回 false
    // 参数：query - SQL 语句
    bool update(const char* query);
    // 同步执行查询操作（SELECT），结果存入 *result（调用方负责 mysql_free_result）；失败返回 false
    // 参数：query - SQL 语句；result - 输出参数，结果集指针
    bool query(const char* query, MYSQL_RES** result);
    // 关闭数据库连接；成功关闭返回 true
    bool close();

    // ---- 异步接口（失败抛 MySQLAsyncError / MySQLTimeoutError，需跑在事件循环内）----
    // 注意：同一 connection 的异步接口不能并发调用——底层复用单一 MYSQL 句柄
    // 与 fd 的非阻塞状态机，并发调用会互相践踏状态（前一个协程挂起期间
    // 第二个协程推进同一状态机是未定义行为）。

    // 异步连接；timeout_ms 为总超时
    coro::Task<void> async_connect(const char* host, const char* user,
                                   const char* password, const char* database,
                                   int64_t timeout_ms = 10000);
    // 异步查询，返回结果集（调用方负责 mysql_free_result）；总超时后抛 MySQLTimeoutError
    coro::Task<MYSQL_RES*> async_query(const char* sql, int64_t timeout_ms = 5000);
    // 异步更新（INSERT/UPDATE/DELETE），返回影响行数
    coro::Task<uint64_t> async_update(const char* sql, int64_t timeout_ms = 5000);

    // 暴露底层句柄（测试/池用）
    MYSQL* raw() { return conn_; }
    // 连接可用性：已连接且未发生网络类错误（池的失效判断用）
    bool is_valid() const { return conn_ != nullptr && valid_; }

private:
    MYSQL* conn_;
    bool valid_ = false;   // 网络类错误（NET_ASYNC_ERROR / 超时）后置 false；同步接口不维护
};