// RPC 服务端：监听 + 方法注册表 + 连接分发。
// Serve() 监听循环 post 到构造时传入的 EventLoop；每连接 spawn 独立处理协程。
// 注册表键为 "pkg.Service/Method"，由框架层手写 dispatch 查表。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "coro/event_loop.h"
#include "coro/task.h"
#include "net/tcp_listener.h"
#include "rpc/codegen.h"

namespace rpc {

class RpcServer {
public:
    explicit RpcServer(coro::EventLoop& loop);
    ~RpcServer();
    RpcServer(const RpcServer&) = delete;             // 禁止拷贝
    RpcServer& operator=(const RpcServer&) = delete;  // 禁止拷贝赋值

    // 开始监听；port=0 由内核分配。成功返回 true
    // 参数：host - 绑定地址；port - 端口；reuse_port - 是否 SO_REUSEPORT
    bool Start(std::string_view host, std::uint16_t port, bool reuse_port = false);

    // 实际绑定端口（Start 成功后有效）
    std::uint16_t port() const { return bound_port_; }

    // 注册一元/流方法处理器；service="pkg.Service", method="SayHello"
    void Register(std::string service, std::string method, MethodHandler mh);

    // 查询处理器；不存在返回 nullptr
    const MethodHandler* FindHandler(const std::string& service,
                                     const std::string& method) const;

    // 接受连接循环（post 到 loop 驱动）；Stop() 后退出
    coro::Task<void> Serve();

    // 停止监听（置标志，Serve 循环退出）
    void Stop() { stopped_ = true; }

private:
    friend class RpcServerConnection;
    coro::EventLoop& loop_;
    net::TcpListener listener_;
    std::map<std::string, MethodHandler> handlers_;  // key: "service/method"
    std::uint16_t bound_port_ = 0;
    bool stopped_ = false;
};

}  // namespace rpc
