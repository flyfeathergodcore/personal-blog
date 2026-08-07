// kqueue 后端（macOS）：EVFILT_READ / EVFILT_WRITE 分别注册
#include "coro/io_poller.h"

#include <sys/event.h>
#include <unistd.h>

#include <cerrno>

namespace coro {
namespace {
constexpr int kMaxEvents = 64;
}  // namespace

class KqueuePoller final : public IoPoller {
public:
    // 构造函数：创建 kqueue 实例
    KqueuePoller() : kq_(kqueue()) {}
    // 析构函数：关闭 kqueue 描述符
    ~KqueuePoller() override {
        if (kq_ >= 0) close(kq_);
    }

    // 注册 fd 监听事件（EVFILT_READ / EVFILT_WRITE 分别注册）
    // 参数：fd - 文件描述符；events - READ/WRITE 事件组合；返回：成功为 true
    bool add(int fd, int events) override { return ctl(fd, events, EV_ADD); }
    // kqueue 中 EV_ADD 兼更新，modify 与 add 相同
    bool modify(int fd, int events) override { return ctl(fd, events, EV_ADD); }

    // 移除 fd 的 READ/WRITE 监听
    // 参数：fd - 文件描述符；返回：成功为 true
    bool remove(int fd) override {
        for (int f = 0; f < 2; ++f) {
            struct kevent ev;
            EV_SET(&ev, fd, f == 0 ? EVFILT_READ : EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        }
        return true;
    }

    // 阻塞等待事件，最多 timeout_ms（-1 = 无限）；超时返回空列表
    // 参数：timeout_ms - 等待毫秒数；返回：就绪事件列表
    std::vector<ReadyEvent> wait(int timeout_ms) override {
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
        if (n <= 0) return out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            int ev = 0;
            if (evs[i].filter == EVFILT_READ) ev |= READ;
            if (evs[i].filter == EVFILT_WRITE) ev |= WRITE;
            out.push_back({static_cast<int>(evs[i].ident), ev});
        }
        return out;
    }

private:
    // 按事件组合对 READ/WRITE 两个 filter 分别执行 ADD 或 DELETE
    // 参数：fd - 文件描述符；events - 事件组合；flags - EV_ADD 等标志；返回：全部成功为 true
    bool ctl(int fd, int events, int flags) {
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

    int kq_;
};

// 平台工厂：返回 kqueue 后端 poller
std::unique_ptr<IoPoller> create_poller() {
    return std::make_unique<KqueuePoller>();
}

}  // namespace coro
