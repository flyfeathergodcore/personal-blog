#pragma once

#include "tcp/accept_strategy.hpp"
#include "coro/task.h"
#include "net/signal_watcher.h"
#include "tcp/channel.hpp"
#include "tcp/reactor.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

/*
TCP 服务器封装（协程式非阻塞 accept）
设计：
    - 构造时传入监听 host/port 与连接处理回调 handler
    - Start() 通过 accept 策略创建监听 socket，并投递 accept 协程。
    - Stop() 停止事件循环，优雅退出
私有：
    - AcceptLoop() 协程循环接受连接，调用 handler_ 处理每个连接
    - SignalLoop() 协程循环等待信号，收到 SIGINT 或 SIGTERM 时调用 Stop() 优雅关闭
*/

namespace tcp {

using ConnectionHandler = std::function<coro::Task<void>(Channel)>;
class Server {
public:
    Server(std::string host, uint16_t port, ConnectionHandler handler,
           AcceptStrategyType accept_strategy = AcceptStrategyType::ReusePort);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int Start();
    void Stop();

private:
    coro::Task<void> AcceptLoop(Listener& listener);
    coro::Task<void> SignalLoop();

    std::string host_;
    uint16_t port_;
    ConnectionHandler handler_;
    Reactor reactor_;
    AcceptStrategyType accept_strategy_type_;
    std::unique_ptr<AcceptStrategy> accept_strategy_;
    net::SignalWatcher signals_;
    std::atomic<bool> stopping_{false};
};

}  // namespace tcp
