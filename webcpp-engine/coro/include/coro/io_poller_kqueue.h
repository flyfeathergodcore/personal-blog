#pragma once

#include "coro/io_poller.h"

namespace coro {

// macOS/BSD kqueue 后端声明；具体系统调用实现位于 src/io_poller_kqueue.cpp。
class KqueuePoller final : public IoPoller {
public:
    KqueuePoller();
    ~KqueuePoller() override;

    bool add(int fd, int events) override;
    bool modify(int fd, int events) override;
    bool remove(int fd) override;
    std::vector<ReadyEvent> wait(int timeout_ms) override;

private:
    bool ctl(int fd, int events, int flags);

    int kq_ = -1;
};

}  // namespace coro
