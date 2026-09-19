/*
TCP 监听 socket 封装（协程式非阻塞 accept）。
设计：
    - 默认构造：fd_=-1，未监听
    - open 创建非阻塞监听 socket 并 bind/listen（支持 IPv4/IPv6 与主机名，
    - accept 用 accept4 产出已 O_NONBLOCK 的连接 fd 交给 Stream，无连接就绪时在事件循环上等 READ 事件，不阻塞线程。
    - close 关闭监听 fd（幂等）
*/
#pragma once

#include <cstdint>
#include <string_view>

#include "coro/task.h"
#include "tcp/stream.hpp"

namespace tcp {

class Listener {
public:
    Listener() = default;                  // 默认构造：fd_=-1，未监听
    ~Listener();                           // 析构关闭 fd
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    bool open(std::string_view host, uint16_t port, bool reuse_port = false);
    coro::Task<IoResult> accept(Stream& out, int64_t timeout_ms = -1);

    void close();
    int  fd() const { return fd_; }
private:
    int fd_ = -1;
};

}  // namespace tcp
