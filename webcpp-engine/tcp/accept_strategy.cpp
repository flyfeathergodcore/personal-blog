#include "tcp/accept_strategy.hpp"

#include "coro/event_loop.h"
#include "tcp/reactor.hpp"

#include <memory>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace tcp {
namespace {

class SingleAcceptStrategy final : public AcceptStrategy {
public:
    bool Open(std::string_view host, uint16_t port, Reactor&) override
    {
        listener_ = std::make_unique<Listener>();
        if (!listener_->open(host, port, false))
        {
            listener_.reset();
            return false;
        }
        return true;
    }

    void Start(Reactor& reactor, const AcceptLoopFactory& make_accept_loop) override
    {
        coro::Task<void> accept = make_accept_loop(*listener_);
        // 单 acceptor 由 base loop 驱动；建立连接后再轮询分发给 worker。
        reactor.Post(accept.handle());
    }

    void DispatchConnection(Reactor& reactor, std::coroutine_handle<> task) override
    {
        reactor.PostWorker(task);
    }

    void Close() override
    {
        if (listener_)
            listener_->close();
    }

    std::size_t AcceptorCount() const override { return listener_ ? 1 : 0; }
    std::string_view Name() const override { return "single"; }

private:
    std::unique_ptr<Listener> listener_;
};

class ReusePortAcceptStrategy final : public AcceptStrategy {
public:
    bool Open(std::string_view host, uint16_t port, Reactor& reactor) override
    {
#ifndef SO_REUSEPORT
        (void)host;
        (void)port;
        (void)reactor;
        return false;
#else
        listeners_.clear();
        const std::size_t count = reactor.WorkerCount();
        listeners_.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto listener = std::make_unique<Listener>();
            if (!listener->open(host, port, true))
            {
                listeners_.clear();
                return false;
            }
            listeners_.push_back(std::move(listener));
        }
        return !listeners_.empty();
#endif
    }

    void Start(Reactor& reactor, const AcceptLoopFactory& make_accept_loop) override
    {
        for (std::size_t i = 0; i < listeners_.size(); ++i)
        {
            coro::Task<void> accept = make_accept_loop(*listeners_[i]);
            // 一个 listener 固定绑定一个 worker，内核负责 SO_REUSEPORT 连接分流。
            reactor.PostWorkerAt(i, accept.handle());
        }
    }

    void DispatchConnection(Reactor&, std::coroutine_handle<> task) override
    {
        // accept 协程已在目标 worker，连接协程继续留在同一 EventLoop。
        coro::EventLoop::current().post(task);
    }

    void Close() override
    {
        for (auto& listener : listeners_)
            listener->close();
    }

    std::size_t AcceptorCount() const override { return listeners_.size(); }
    std::string_view Name() const override { return "reuseport"; }

private:
    std::vector<std::unique_ptr<Listener>> listeners_;
};

}  // namespace

std::unique_ptr<AcceptStrategy> CreateAcceptStrategy(AcceptStrategyType type)
{
    switch (type)
    {
    case AcceptStrategyType::Single:
        return std::make_unique<SingleAcceptStrategy>();
    case AcceptStrategyType::ReusePort:
        return std::make_unique<ReusePortAcceptStrategy>();
    }
    return std::make_unique<SingleAcceptStrategy>();
}

}  // namespace tcp
