// epoll 后端（Linux）：EPOLLIN / EPOLLOUT 映射
#include "coro/io_poller.h"

#include <sys/epoll.h>
#include <unistd.h>

namespace coro {
namespace {
constexpr int kMaxEvents = 64;
}  // namespace

class EpollPoller final : public IoPoller {
public:
    EpollPoller() : epfd_(epoll_create1(0)) {}
    ~EpollPoller() override {
        if (epfd_ >= 0) close(epfd_);
    }

    bool add(int fd, int events) override { return ctl(fd, events, EPOLL_CTL_ADD); }
    bool modify(int fd, int events) override { return ctl(fd, events, EPOLL_CTL_MOD); }
    bool remove(int fd) override {
        struct epoll_event ev{};
        return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &ev) == 0;
    }

    std::vector<ReadyEvent> wait(int timeout_ms) override {
        struct epoll_event evs[kMaxEvents];
        int n = epoll_wait(epfd_, evs, kMaxEvents, timeout_ms);
        std::vector<ReadyEvent> out;
        if (n <= 0) return out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            int ev = 0;
            if (evs[i].events & EPOLLIN) ev |= READ;
            if (evs[i].events & EPOLLOUT) ev |= WRITE;
            out.push_back({static_cast<int>(evs[i].data.fd), ev});
        }
        return out;
    }

private:
    bool ctl(int fd, int events, int op) {
        struct epoll_event ev{};
        if (events & READ) ev.events |= EPOLLIN;
        if (events & WRITE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(epfd_, op, fd, &ev) == 0;
    }

    int epfd_;
};

std::unique_ptr<IoPoller> create_poller() {
    return std::make_unique<EpollPoller>();
}

}  // namespace coro
