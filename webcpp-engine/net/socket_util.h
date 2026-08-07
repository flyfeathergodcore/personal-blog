// socket 创建/接受的跨平台辅助：统一产出「O_NONBLOCK + FD_CLOEXEC」的 fd。
// Linux 用 SOCK_NONBLOCK|SOCK_CLOEXEC 内核 flag 和 accept4（一步到位）；
// macOS/BSD 无这些，用 accept + fcntl 等价设置。TcpStream 要求 fd 已非阻塞。
#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net {

// 创建已设 O_NONBLOCK + FD_CLOEXEC 的 socket；失败返回 -1（errno 已设置）
// 参数：family/type/proto - 透传 socket() 的参数
inline int SocketNonBlockCloexec(int family, int type, int proto) {
#ifdef __linux__
    return ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
#else
    int fd = ::socket(family, type, proto);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        int fl = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return fd;
#endif
}

// accept 出已 O_NONBLOCK + FD_CLOEXEC 的连接 fd；失败返回 -1（errno 已设置）
inline int AcceptNonBlockCloexec(int listen_fd) {
#ifdef __linux__
    return ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int cfd = ::accept(listen_fd, nullptr, nullptr);
    if (cfd >= 0) {
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
        int fl = ::fcntl(cfd, F_GETFL, 0);
        ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
    }
    return cfd;
#endif
}

}  // namespace net
