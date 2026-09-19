#pragma once

#include "coro/io_poller.h"

namespace coro {

// Linux epoll 后端声明；具体系统调用实现位于 src/io_poller_epoll.cpp。
class EpollPoller final : public IoPoller {
public:
    EpollPoller();
    ~EpollPoller() override;

    bool add(int fd, int events) override;
    bool add_level(int fd, int events) override;
    bool modify(int fd, int events) override;
    bool remove(int fd) override;
    std::vector<ReadyEvent> wait(int timeout_ms) override;

private:
    int epfd_ = -1;
};

}  // namespace coro
