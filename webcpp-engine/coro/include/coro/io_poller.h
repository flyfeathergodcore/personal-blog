// IO 多路复用抽象：注册 fd 与事件，等待就绪。无状态——只报"哪个 fd 就绪了"
#pragma once

#include <memory>
#include <vector>

namespace coro {

class IoPoller {
    /*
    IoPoller 用途：
    1. IO 多路复用抽象：注册 fd 与事件，等待就绪。无状态——只报"哪个 fd 就绪了"
    2. 提供 add/modify/remove 接口，允许注册 fd 及其感兴趣的事件（读/写）
    3. 提供 wait 接口，阻塞等待就绪事件，并返回就绪的 fd 列表
    4. 提供 add_level 接口，允许注册 fd 的 level-triggered 事件（可选，默认使用 edge-triggered）
    5. 提供枚举 Event，表示可注册的事件类型（READ/WRITE）
    6. 提供友元函数 operator|，允许组合事件类型（如 READ | WRITE）
    7. 提供虚析构函数，确保派生类（epoll/kqueue）正确清理资源
    8. 提供纯虚函数 add/modify/remove/wait，派生类必须实现这些接口
    9. 提供静态工厂函数 create_poller，根据平台创建对应的 IoPoller 实现（Linux → epoll，macOS → kqueue）
    10. 提供结构体 ReadyEvent，表示就绪的 fd 及其事件组合
    11. 提供虚函数 add_level，允许派生类实现 level-triggered 事件注册（默认调用 add）
    */
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

    // 虚析构函数：保证派生类（epoll/kqueue）正确清理
    virtual ~IoPoller() = default;
    virtual bool add(int fd, int events) = 0;
    virtual bool add_level(int fd, int events) { return add(fd, events); }
    virtual bool modify(int fd, int events) = 0;
    virtual bool remove(int fd) = 0;
    virtual std::vector<ReadyEvent> wait(int timeout_ms) = 0;
};

// 平台工厂：Linux → epoll，macOS → kqueue
std::unique_ptr<IoPoller> create_poller();

}  // namespace coro
