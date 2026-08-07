// TcpListener 实现：getaddrinfo 解析主机名/端口 → socket(NONBLOCK|CLOEXEC)
// → setsockopt(SO_REUSEADDR[/SO_REUSEPORT]) → bind → listen(1024)。
// socket 创建与 accept 走跨平台 helper（net/socket_util.h）：返回的连接 fd
// 天然满足 TcpStream 的"fd 已 O_NONBLOCK"前置条件。
#include "net/tcp_listener.h"
#include "net/socket_util.h"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coro/awaiter.h"

namespace net {

// 析构：关闭监听 fd
TcpListener::~TcpListener() { close(); }

// 创建并绑定监听 socket：getaddrinfo → socket(NONBLOCK|CLOEXEC) → bind → listen(1024)
// 参数：host - 绑定地址（空/nullptr 绑定通配地址）；port - 端口（0 由内核分配）；reuse_port - 是否设置 SO_REUSEPORT
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

// 协程接受一条连接：accept4 产出已 O_NONBLOCK 的连接 fd 移交给 out
// 参数：out - 接收连接（覆盖旧值）；timeout_ms - 等待超时（毫秒，<0 无限）
coro::Task<IoResult> TcpListener::accept(TcpStream& out, int64_t timeout_ms) {
    if (fd_ < 0) co_return IoResult{0, IoError::Closed};
    for (;;) {
        // 产出 O_NONBLOCK|FD_CLOEXEC 的连接 fd（Linux 用 accept4，macOS 用 fcntl）
        int cfd = AcceptNonBlockCloexec(fd_);
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

// 关闭监听 fd（幂等）
void TcpListener::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

}  // namespace net
