// Listener 实现：getaddrinfo 解析主机名/端口 → socket(NONBLOCK|CLOEXEC)
// → setsockopt(SO_REUSEADDR[/SO_REUSEPORT]) → bind → listen(1024)。
// socket 创建与 accept 走跨平台 helper（tcp/socket_util.hpp）：返回的连接 fd
// 天然满足 Stream 的"fd 已 O_NONBLOCK"前置条件。
#include "tcp/listener.hpp"
#include "tcp/socket_util.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coro/awaiter.h"

namespace tcp {

// 析构：关闭监听 fd
Listener::~Listener() { close(); }

bool Listener::open(std::string_view host, uint16_t port, bool reuse_port) {
/*
1. 关闭旧 fd（幂等）
2. 配置addrinfo选项 使用getaddrinfo解析主机名和端口，返回链表addrinfo*
    - AF_UNSPEC: 支持IPv4和IPv6 / AF_INET: 仅支持IPv4 / AF_INET6: 仅支持IPv6 / AF_UNIX: 本地套接字
    - SOCK_STREAM: TCP流式套接字 / SOCK_DGRAM  UDP数据报套接字
    - AI_PASSIVE: 用于服务器端，允许绑定到通配地址
3. 遍历解析结果，尝试创建socket并设置选项
4. 绑定socket并监听，如果成功则返回true，否则关闭socket并继续尝试
5. 如果所有尝试都失败，释放addrinfo资源并返回false
*/
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    const std::string address(host);
    const char* node = address.empty() ? nullptr : address.c_str();
    if (getaddrinfo(node, port_s.c_str(), &hints, &res) != 0)
        return false;

    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = SocketNonBlockCloexec(ai->ai_family, ai->ai_socktype,
                                       ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        // 总开 SO_REUSEADDR，避免 TIME_WAIT 残留导致重启监听失败
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        if (reuse_port)
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            ::listen(fd, 1024) == 0) {
            freeaddrinfo(res);
            fd_ = fd;
            return true;
        }
        ::close(fd);
    }
    freeaddrinfo(res);
    return false;
}

coro::Task<IoResult> Listener::accept(Stream& out, int64_t timeout_ms) {
    /*
    1. 判断监听 fd 是否有效，如果无效则返回 IoResult{0, IoError::Closed}
    2. 协程接受一条连接：accept4 产出已 O_NONBLOCK 的连接 fd 移交给 out
     - 如果 accept4 成功，返回 IoResult{0, IoError::None}
     - 如果 accept4 返回 EINTR，继续尝试
     - 如果 accept4 返回 EAGAIN 或 EWOULDBLOCK，协程挂起等待 fd 的 READ 事件，超时返回 IoResult{0, IoError::Timeout}
     - 如果 accept4 返回其他错误，返回 IoResult{0, IoError::Other}
    3. 参数说明：
     - out: 接收连接（覆盖旧值）
     - timeout_ms: 等待超时（毫秒，<0 无限）
    */
    if (fd_ < 0) co_return IoResult{0, IoError::Closed};
    for (;;) {
        int cfd = AcceptNonBlockCloexec(fd_);
        if (cfd >= 0) {
            out = Stream(cfd);
            co_return IoResult{0, IoError::None};
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto ready = co_await coro::await_event(fd_, coro::IoPoller::READ, timeout_ms);
            if (ready == coro::Readiness::Timeout)
                co_return IoResult{0, IoError::Timeout};
            continue;
        }
        co_return IoResult{0, IoError::Other};
    }
}

// 关闭监听 fd（幂等）
void Listener::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

}  // namespace tcp
