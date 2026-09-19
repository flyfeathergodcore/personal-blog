// kqueue 后端（macOS）：EVFILT_READ / EVFILT_WRITE 分别注册。
#include "coro/io_poller_kqueue.h"

#include <sys/event.h>
#include <unistd.h>

#include <cerrno>

namespace coro {
namespace {
constexpr int kMaxEvents = 64;
}  // namespace

KqueuePoller::KqueuePoller() : kq_(kqueue()) {}

KqueuePoller::~KqueuePoller()
{
    if (kq_ >= 0)
        close(kq_);
}

bool KqueuePoller::add(int fd, int events)
{
    return ctl(fd, events, EV_ADD);
}

bool KqueuePoller::modify(int fd, int events)
{
    return ctl(fd, events, EV_ADD);
}

bool KqueuePoller::remove(int fd)
{
    for (int f = 0; f < 2; ++f) {
        struct kevent ev;
        EV_SET(&ev, fd, f == 0 ? EVFILT_READ : EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }
    return true;
}

std::vector<IoPoller::ReadyEvent> KqueuePoller::wait(int timeout_ms)
{
    struct timespec ts;
    struct timespec* pts = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    struct kevent evs[kMaxEvents];
    int n = kevent(kq_, nullptr, 0, evs, kMaxEvents, pts);
    std::vector<ReadyEvent> out;
    if (n <= 0)
        return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        int ev = 0;
        if (evs[i].filter == EVFILT_READ)
            ev |= READ;
        if (evs[i].filter == EVFILT_WRITE)
            ev |= WRITE;
        out.push_back({static_cast<int>(evs[i].ident), ev});
    }
    return out;
}

bool KqueuePoller::ctl(int fd, int events, int flags)
{
    bool ok = true;
    for (int f = 0; f < 2; ++f) {
        const short filter = f == 0 ? EVFILT_READ : EVFILT_WRITE;
        const bool want = f == 0 ? (events & READ) != 0 : (events & WRITE) != 0;
        struct kevent ev;
        if (want) {
            EV_SET(&ev, fd, filter, flags, 0, 0, nullptr);
            if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) == -1 && errno != EEXIST)
                ok = false;
        } else {
            EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, nullptr);
            kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        }
    }
    return ok;
}

std::unique_ptr<IoPoller> create_poller()
{
    return std::make_unique<KqueuePoller>();
}

}  // namespace coro
