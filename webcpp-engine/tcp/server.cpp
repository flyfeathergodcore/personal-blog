#include "tcp/server.hpp"
#include "tcp/tcp_log.hpp"

#include "coro/awaiter.h"

#include <csignal>
#include <utility>

namespace tcp
{

    Server::Server(std::string host, uint16_t port, ConnectionHandler handler,
                   AcceptStrategyType accept_strategy)
        : host_(std::move(host)), port_(port), handler_(std::move(handler)),
          accept_strategy_type_(accept_strategy) {}

    int Server::Start()
    {
        /*
        1. 判定handler_，handler_是用户定义的回调函数，用于处理每个tcp连接进来后，对tcp连接所作的处理逻辑
        2. 忽略SIGPIPE信号，防止写入已关闭的socket时程序被终止
        3. 初始化信号监听器signals_，监听SIGINT和SIGTERM信号，用于优雅退出程序
        4. 由 accept 策略创建监听 socket；ReusePort 策略为每个 worker 创建一个 socket
        5. 创建信号协程并投递到 base loop
        6. worker 启动后，将每个 accept 协程投递到对应 worker loop
        7. 启动 reactor，base loop 只负责信号等控制事件
        8. 返回0，表示程序正常退出
        */
        InitLogging();
        if (!handler_)
        {
            Log(LogLevel::Error, "TCP_SERVER", "connection handler is required");
            return 2;
        }

        ::signal(SIGPIPE, SIG_IGN);
        if (!signals_.init({SIGINT, SIGTERM}))
        {
            Log(LogLevel::Error, "TCP_SERVER", "signal watcher initialization failed");
            return 1;
        }
        accept_strategy_ = CreateAcceptStrategy(accept_strategy_type_);
        if (!accept_strategy_->Open(host_, port_, reactor_))
        {
            if (accept_strategy_type_ == AcceptStrategyType::Single)
            {
                Log(LogLevel::Error, "TCP_SERVER",
                    "failed to listen on " + host_ + ':' + std::to_string(port_));
                return 1;
            }

            // ReusePort 不可用时由工厂切换为单 acceptor 策略，保证服务可启动。
            Log(LogLevel::Warning, "TCP_SERVER",
                "SO_REUSEPORT acceptors unavailable; falling back to single acceptor");
            accept_strategy_ = CreateAcceptStrategy(AcceptStrategyType::Single);
            if (!accept_strategy_->Open(host_, port_, reactor_))
            {
                Log(LogLevel::Error, "TCP_SERVER",
                    "failed to listen on " + host_ + ':' + std::to_string(port_));
                return 1;
            }
        }

        Log(LogLevel::Info, "TCP_SERVER",
            "listening on " + host_ + ':' + std::to_string(port_) +
                " with " + std::to_string(accept_strategy_->AcceptorCount()) +
                " " + std::string(accept_strategy_->Name()) + " acceptor(s)");
        coro::Task<void> signal = SignalLoop();
        reactor_.Post(signal.handle());
        reactor_.Run([this] {
            accept_strategy_->Start(reactor_, [this](Listener& listener) {
                return AcceptLoop(listener);
            });
        });
        return 0;
    }

    void Server::Stop()
    {
        if (stopping_.exchange(true))
            return;
        if (accept_strategy_)
            accept_strategy_->Close();
        reactor_.Stop();
    }

    coro::Task<void> Server::AcceptLoop(Listener& listener)
    {
        /*
        1. 如果没有停止标志，创建一个Stream对象
        2. 调用 listener.accept(stream) 等待连接，如果成功，覆盖 stream 对象的 fd
        3. 如果accept失败，检查是否停止，如果没有停止，等待10毫秒后继续循环
        4. 如果停止标志被设置，关闭stream并退出循环
        5. 如果 accept 成功，调用 handler_；后续投递方式由 accept 策略决定
        */
        while (!stopping_)
        {
            Stream stream;
            const auto result = co_await listener.accept(stream);
            if (!result.ok())
            {
                if (!stopping_)
                    co_await coro::sleep_for(10);
                continue;
            }
            if (stopping_)
            {
                stream.close();
                break;
            }

            coro::Task<void> client = handler_(Channel(std::move(stream)));
            accept_strategy_->DispatchConnection(reactor_, client.handle());
        }
        co_return;
    }

    coro::Task<void> Server::SignalLoop()
    {
        /*
        1. 初始化信号循环
        2. 等待信号
        3. 如果收到信号，异步记录信息并停止服务器
        */
        while (true)
        {
            const int signal = co_await signals_.wait();
            if (signal < 0)
                continue;
            Log(LogLevel::Info, "TCP_SERVER",
                "received signal " + std::to_string(signal) + ", stopping");
            Stop();
            break;
        }
        co_return;
    }

} // namespace tcp
