// epoll 后端（Linux）：EPOLLIN / EPOLLOUT 映射。
// 模式：edge-triggered（EPOLLET）。
#include "coro/io_poller_epoll.h"

#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>

namespace coro {
namespace {
constexpr int kMaxEvents = 64;
}  // namespace

EpollPoller::EpollPoller() : epfd_(epoll_create1(0)) {}

EpollPoller::~EpollPoller()
{
    if (epfd_ >= 0)
        close(epfd_);
}

bool EpollPoller::add(int fd, int events)
{
    struct epoll_event ev{};
    if (events & READ)
        ev.events |= EPOLLIN;
    if (events & WRITE)
        ev.events |= EPOLLOUT;
    ev.events |= EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0)
        return true;
    if (errno != ENOENT)
        return false;
    return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EpollPoller::add_level(int fd, int events)
{
    struct epoll_event ev{};
    if (events & READ)
        ev.events |= EPOLLIN;
    if (events & WRITE)
        ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EpollPoller::modify(int fd, int events)
{
    struct epoll_event ev{};
    if (events & READ)
        ev.events |= EPOLLIN;
    if (events & WRITE)
        ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    return epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EpollPoller::remove(int fd)
{
    struct epoll_event ev{};
    return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &ev) == 0;
}

std::vector<IoPoller::ReadyEvent> EpollPoller::wait(int timeout_ms)
{
    struct epoll_event evs[kMaxEvents];
    int n = epoll_wait(epfd_, evs, kMaxEvents, timeout_ms);
    std::vector<ReadyEvent> out;
    if (n <= 0)
        return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        int ev = 0;
        if (evs[i].events & EPOLLIN)
            ev |= READ;
        if (evs[i].events & EPOLLOUT)
            ev |= WRITE;
        out.push_back({static_cast<int>(evs[i].data.fd), ev});
    }
    return out;
}

std::unique_ptr<IoPoller> create_poller()
{
    return std::make_unique<EpollPoller>();
}

}  // namespace coro
