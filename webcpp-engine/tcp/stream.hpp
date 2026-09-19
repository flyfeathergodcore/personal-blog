/*
    stream.hpp
    TCP 连接封装：对操作系统 TCP socket 的协程式封装
    设计：
        - 构造时传入 fd，析构时关闭 fd
        - read_some() / read_exact() / read_until() / write_all() / writev_all() 是协程式读写
        - Stream 自己持有预读缓冲，保证按分隔符读取时不会丢失多读出的 TCP 字节
        - is_open() / fd() / close() 提供 fd 生命周期管理  
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "coro/task.h"

namespace tcp {

enum class IoError { None, Eof, Closed, Timeout, Other };
struct IoResult {
    size_t bytes = 0;
    IoError err = IoError::None;
    bool ok() const { return err == IoError::None; }
};

class Stream {
public:
    Stream() = default;
    explicit Stream(int fd);
    ~Stream();

    Stream(Stream&&) noexcept;
    Stream& operator=(Stream&&) noexcept;

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    coro::Task<IoResult> read_some(void* buf, size_t n, int64_t timeout_ms = -1);
    coro::Task<IoResult> read_exact(void* buf, size_t n, int64_t timeout_ms = -1);
    // 从 Stream 内部缓冲中按分隔符读取；分隔符可跨越多次 socket read。
    coro::Task<IoResult> read_until(std::string_view delim, std::string& out,
                                    int64_t timeout_ms = -1);
    // 读取 n 个字节到 string；优先消费内部预读缓冲。
    coro::Task<IoResult> read_exact(size_t n, std::string& out,
                                    int64_t timeout_ms = -1);
    // 查看尚未消费的预读数据；视图在下一次 Stream 读操作后可能失效。
    std::string_view buffered() const;
    // 移出并消费全部预读数据，适合把协议升级后的首帧交给其他读取器。
    std::string take_buffered();
    // 设置内部读取缓冲的预留容量；不会丢弃已经缓冲的数据。
    void reserve_read_buffer(size_t capacity);
    coro::Task<bool>    write_all(std::string_view data, int64_t timeout_ms = -1);
    coro::Task<bool>    writev_all(std::initializer_list<std::string_view> parts,
                                   int64_t timeout_ms = -1);


    bool is_open() const { return fd_ >= 0; }
    int  fd() const { return fd_; }
    void close();
private:
    coro::Task<IoResult> read_raw(void* buf, size_t n, int64_t timeout_ms);
    void compact_buffer();

    int fd_ = -1;
    std::string read_buffer_;
    size_t read_pos_ = 0;
};

}  // namespace tcp
