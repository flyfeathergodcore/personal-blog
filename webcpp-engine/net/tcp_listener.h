// net 层监听器：TcpListener 封装（协程式非阻塞 accept）。
// 设计：open 创建非阻塞监听 socket 并 bind/listen（支持 IPv4/IPv6 与主机名，
// 走 getaddrinfo）；accept 用 accept4 产出已 O_NONBLOCK 的连接 fd 交给 TcpStream，
// 无连接就绪时在事件循环上等 READ 事件，不阻塞线程。
#pragma once

#include <cstdint>

#include "coro/task.h"
#include "net/tcp_stream.h"

namespace net {

class TcpListener {
public:
    TcpListener() = default;               // 默认构造：fd_=-1，未监听
    // 析构：关闭监听 fd
    ~TcpListener();                        // 析构关闭 fd
    // 禁止拷贝
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // 创建并监听：host 为空/nullptr 时绑定通配地址；port=0 由内核分配端口。
    // reuse_port 为 true 时设置 SO_REUSEPORT。任一失败返回 false（fd 已关闭）。
    bool open(const char* host, uint16_t port, bool reuse_port);

    // 接受一条连接，成功时把连接 fd 移交给 out（覆盖其旧值）。
    // 无连接等待超过 timeout_ms 返回 IoResult{0, Timeout}；
    // 成功返回 IoResult{0, None}（无数据负载，ok()==true）。
    coro::Task<IoResult> accept(TcpStream& out, int64_t timeout_ms = -1);

    // 关闭监听 fd（幂等）
    void close();                          // 幂等
    // 返回监听 fd（未监听为 -1）
    int  fd() const { return fd_; }
private:
    int fd_ = -1;
};

}  // namespace net
