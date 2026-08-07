// epoll 后端（Linux）：EPOLLIN / EPOLLOUT 映射
// 模式：edge-triggered（EPOLLET）。
//   - add()：MOD 优先重挂（fd 已注册的 keep-alive 复用 / 事件掩码变更），
//     新 fd / 复用号（旧注册已被内核随 close 清除）MOD→ENOENT→ADD。
//     就绪后事件循环不再 remove——ET 只在状态翻转时触发，消费方读到 EAGAIN
//     后重新 add 挂起，天然无 level-triggered 忙转。每等待 1 次 epoll_ctl
//     （原 LT 模式 wait_io 的 ADD + run 的 DEL 共 2 次）。
//   - add_level()：唤醒管道专用，保持 level-triggered——多线程 run() 下
//     需要 wake 字节持续就绪，让所有等待线程的 epoll_wait 都能返回。
#include "coro/io_poller.h"

#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>

namespace coro {
namespace {
constexpr int kMaxEvents = 64;
}  // namespace

class EpollPoller final : public IoPoller {
public:
    // 构造函数：创建 epoll 实例
    EpollPoller() : epfd_(epoll_create1(0)) {}
    // 析构函数：关闭 epoll 描述符
    ~EpollPoller() override {
        if (epfd_ >= 0) close(epfd_);
    }

    // 注册 fd 监听事件（ET 模式）；已注册则 MOD 重挂，新 fd/复用号走 ADD
    // 参数：fd - 文件描述符；events - READ/WRITE 事件组合；返回：成功为 true
    bool add(int fd, int events) override {
        struct epoll_event ev{};
        if (events & READ) ev.events |= EPOLLIN;
        if (events & WRITE) ev.events |= EPOLLOUT;
        ev.events |= EPOLLET;
        ev.data.fd = fd;
        // 已注册（同连接重复等待 / 事件掩码变更）：MOD 重挂（含 ET），1 次 syscall。
        // 新 fd / fd 号复用（旧注册已被内核随 ::close 清除）：MOD→ENOENT→ADD。
        // EPOLLERR/EPOLLHUP 由内核无条件上报，无需显式注册。
        if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) return true;
        if (errno != ENOENT) return false;
        return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    // 唤醒管道：LT（不设 EPOLLET）。多线程 run() 下 wake() 写满管道使事件
    // 持续就绪，所有等待线程的 epoll_wait 都能立即返回；ET 只唤醒一个等待者。
    bool add_level(int fd, int events) override {
        struct epoll_event ev{};
        if (events & READ) ev.events |= EPOLLIN;
        if (events & WRITE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    // 修改 fd 监听事件
    // 参数：fd - 文件描述符；events - 新事件组合；返回：成功为 true
    bool modify(int fd, int events) override {
        struct epoll_event ev{};
        if (events & READ) ev.events |= EPOLLIN;
        if (events & WRITE) ev.events |= EPOLLOUT;
        ev.data.fd = fd;
        return epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    // 移除 fd 监听
    // 参数：fd - 文件描述符；返回：成功为 true
    bool remove(int fd) override {
        struct epoll_event ev{};
        return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &ev) == 0;
    }

    // 阻塞等待事件，最多 timeout_ms（-1 = 无限）；超时返回空列表
    // 参数：timeout_ms - 等待毫秒数；返回：就绪事件列表
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
    int epfd_;
};

// 平台工厂：返回 epoll 后端 poller
std::unique_ptr<IoPoller> create_poller() {
    return std::make_unique<EpollPoller>();
}

}  // namespace coro
