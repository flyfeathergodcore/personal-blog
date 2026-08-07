// net 层基础 I/O：TcpStream 封装 + IoResult（协程可 await 的读写原语）
// 设计：fd 已设为 O_NONBLOCK；读写在 EventLoop 上等就绪事件，不阻塞线程。
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "coro/task.h"

namespace net {

enum class IoError { None, Eof, Closed, Timeout, Other };
struct IoResult {
    size_t bytes = 0;
    IoError err = IoError::None;
    bool ok() const { return err == IoError::None; }
};

class TcpStream {
public:
    TcpStream() = default;                   // 空壳（fd_=-1），供 accept(out) 用
    explicit TcpStream(int fd);              // fd 已 O_NONBLOCK；析构关闭
    ~TcpStream();
    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    coro::Task<IoResult> read_some(void* buf, size_t n, int64_t timeout_ms = -1);
    coro::Task<IoResult> read_exact(void* buf, size_t n, int64_t timeout_ms = -1);
    coro::Task<bool>    write_all(std::string_view data, int64_t timeout_ms = -1);

    /// 单次 writev 写出多个不连续段（如 h1 响应头 + 零拷贝 body），
    /// 不拷贝用户态数据——iovec 直接指向各段（region / FileCache）。
    /// 各 string_view 在协程调用期间必须保持有效（调用方帧稳定，均满足）。
    coro::Task<bool>    writev_all(std::initializer_list<std::string_view> parts,
                                   int64_t timeout_ms = -1);

    bool is_open() const { return fd_ >= 0; }
    int  fd() const { return fd_; }
    void close();                            // 幂等
private:
    int fd_ = -1;
};

}  // namespace net
