// resolve/connect 实现：统一走 getaddrinfo（AF_UNSPEC, SOCK_STREAM）。
// resolve 把候选地址格式化为 Endpoint 列表；connect 在事件循环上做非阻塞
// 多地址遍历，任一地址握手成功即返回，全部失败/超时返回 nullptr。
#include "net/resolver.h"

#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coro/awaiter.h"

namespace net {

std::vector<Endpoint> resolve(std::string_view host, uint16_t port) {
    std::vector<Endpoint> out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string host_s(host);
    std::string port_s = std::to_string(port);
    if (getaddrinfo(host_s.c_str(), port_s.c_str(), &hints, &res) != 0)
        return out;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        char hbuf[NI_MAXHOST];
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, hbuf, sizeof(hbuf),
                        nullptr, 0, NI_NUMERICHOST) != 0)
            continue;
        uint16_t p = 0;
        if (ai->ai_addr->sa_family == AF_INET) {
            p = ntohs(reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_port);
        } else if (ai->ai_addr->sa_family == AF_INET6) {
            p = ntohs(reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_port);
        }
        out.push_back(Endpoint{std::string(hbuf), p});
    }
    freeaddrinfo(res);
    return out;
}

coro::Task<std::unique_ptr<TcpStream>> connect(std::string_view host, uint16_t port,
                                               int64_t timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    if (getaddrinfo(std::string(host).c_str(), port_s.c_str(), &hints, &res) != 0)
        co_return nullptr;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family,
                          ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          ai->ai_protocol);
        if (fd < 0) continue;
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            freeaddrinfo(res);
            co_return std::make_unique<TcpStream>(fd);
        }
        if (rc < 0 && errno == EINPROGRESS) {
            auto ready = co_await coro::await_event(fd, coro::IoPoller::WRITE, timeout_ms);
            if (ready == coro::Readiness::Ready) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                if (soerr == 0) {
                    freeaddrinfo(res);
                    co_return std::make_unique<TcpStream>(fd);
                }
            }
        }
        ::close(fd);
    }
    freeaddrinfo(res);
    co_return nullptr;
}

}  // namespace net
