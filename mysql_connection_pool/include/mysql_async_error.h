// MySQL 异步接口的异常类型：失败抛 MySQLAsyncError（携带 mysql_error 信息），超时抛 MySQLTimeoutError
#pragma once

#include <stdexcept>
#include <string>

class MySQLAsyncError : public std::runtime_error {
public:
    MySQLAsyncError(const std::string& msg, unsigned int err_no)
        : std::runtime_error(msg), err_no_(err_no) {}

    unsigned int errno_val() const { return err_no_; }

private:
    unsigned int err_no_;
};

// 超时子类（err_no_ = 0）
class MySQLTimeoutError : public MySQLAsyncError {
public:
    explicit MySQLTimeoutError(const std::string& msg)
        : MySQLAsyncError(msg, 0) {}
};
