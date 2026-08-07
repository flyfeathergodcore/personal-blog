// SignalWatcher：signalfd 实现（按任务简报逐字实现）。
#include "net/signal_watcher.h"

#include "coro/awaiter.h"

#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <sys/signalfd.h>

namespace net {

bool SignalWatcher::init(const std::vector<int>& sigs) {
    sigset_t set;
    sigemptyset(&set);
    for (int s : sigs) sigaddset(&set, s);
    sigprocmask(SIG_BLOCK, &set, nullptr);          // 必须在 signalfd 创建前 block
    fd_ = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    return fd_ >= 0;
}

coro::Task<int> SignalWatcher::wait() {
    struct signalfd_siginfo info;
    for (;;) {
        auto r = co_await coro::await_readable(fd_);
        (void)r;
        ssize_t n = ::read(fd_, &info, sizeof(info));
        if (n == (ssize_t)sizeof(info)) co_return (int)info.ssi_signo;
        if (n < 0 && errno == EAGAIN) continue;
        co_return -1;
    }
}

void SignalWatcher::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

}  // namespace net
