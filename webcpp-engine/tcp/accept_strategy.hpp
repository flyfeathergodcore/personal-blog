#pragma once

#include "coro/task.h"
#include "tcp/listener.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace tcp {

class Reactor;

// 单监听接收后分发，或每个 worker 使用 SO_REUSEPORT 原地接收。
enum class AcceptStrategyType {
    Single,
    ReusePort,
};

using AcceptLoopFactory = std::function<coro::Task<void>(Listener&)>;

class AcceptStrategy {
public:
    virtual ~AcceptStrategy() = default;

    virtual bool Open(std::string_view host, uint16_t port, Reactor& reactor) = 0;
    virtual void Start(Reactor& reactor, const AcceptLoopFactory& make_accept_loop) = 0;
    virtual void DispatchConnection(Reactor& reactor, std::coroutine_handle<> task) = 0;
    virtual void Close() = 0;
    virtual std::size_t AcceptorCount() const = 0;
    virtual std::string_view Name() const = 0;
};

// 集中策略构造，Server 不依赖具体 listener 分发实现。
std::unique_ptr<AcceptStrategy> CreateAcceptStrategy(AcceptStrategyType type);

}  // namespace tcp
