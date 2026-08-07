// IO 多路复用抽象：注册 fd 与事件，等待就绪。无状态——只报"哪个 fd 就绪了"
#pragma once

#include <memory>
#include <vector>

namespace coro {

class IoPoller {
public:
    enum Event : int { READ = 0x1, WRITE = 0x2 };
    // 事件位或：允许 await_event 等直接传组合事件（如 READ | WRITE）
    friend constexpr Event operator|(Event a, Event b) {
        return static_cast<Event>(static_cast<int>(a) | static_cast<int>(b));
    }

    struct ReadyEvent {
        int fd;
        int events;  // Event 组合
    };

    virtual ~IoPoller() = default;

    // 注册 fd 监听事件；失败返回 false
    virtual bool add(int fd, int events) = 0;
    // 修改 fd 监听事件
    virtual bool modify(int fd, int events) = 0;
    // 移除 fd 监听
    virtual bool remove(int fd) = 0;
    // 阻塞等待事件，最多 timeout_ms（-1 = 无限）；超时返回空列表
    virtual std::vector<ReadyEvent> wait(int timeout_ms) = 0;
};

// 平台工厂：Linux → epoll，macOS → kqueue
std::unique_ptr<IoPoller> create_poller();

}  // namespace coro
