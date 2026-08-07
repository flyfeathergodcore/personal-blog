// TcpListener 实现：getaddrinfo 解析主机名/端口 → socket(SOCK_NONBLOCK|CLOEXEC)
// → setsockopt(SO_REUSEADDR[/SO_REUSEPORT]) → bind → listen(1024)。
// accept 走 accept4(SOCK_NONBLOCK|CLOEXEC)：返回的连接 fd 天然满足
// TcpStream 的"fd 已 O_NONBLOCK"前置条件，免去额外 fcntl。
#include "net/tcp_listener.h"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coro/awaiter.h"

namespace net {

TcpListener::~TcpListener() { close(); }

bool TcpListener::open(const char* host, uint16_t port, bool reuse_port) {
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;        // 按 host 支持 IPv4/IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;        // host 为空时绑定通配地址
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    const char* node = (host && host[0]) ? host : nullptr;
    if (getaddrinfo(node, port_s.c_str(), &hints, &res) != 0)
        return false;

    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family,
                          ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
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

coro::Task<IoResult> TcpListener::accept(TcpStream& out, int64_t timeout_ms) {
    if (fd_ < 0) co_return IoResult{0, IoError::Closed};
    for (;;) {
        // accept4 直接产出 O_NONBLOCK|FD_CLOEXEC 的连接 fd
        int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd >= 0) {
            out = TcpStream(cfd);        // 移动接管连接 fd
            co_return IoResult{0, IoError::None};
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto ready = co_await coro::await_event(fd_, coro::IoPoller::READ, timeout_ms);
            if (ready == coro::Readiness::Timeout)
                co_return IoResult{0, IoError::Timeout};
            continue;                    // 就绪后重试；可挂起在挂起点，重新进入循环
        }
        co_return IoResult{0, IoError::Other};   // 监听 fd 已关闭 / 其他错误
    }
}

void TcpListener::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

}  // namespace net
