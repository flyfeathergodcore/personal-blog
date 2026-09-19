// RpcServer 实现：监听/实际端口查询/注册表/accept 循环
#include "rpc/rpc_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <utility>

#include "coro/awaiter.h"
#include "rpc/rpc_connection.h"

namespace rpc {

namespace {
// 连接处理协程：持有 shared_ptr 保活连接，避免连接在处理中途被析构
static coro::Task<void> RunConn(std::shared_ptr<RpcServerConnection> conn) {
    co_await conn->Serve();
}
}  // namespace

RpcServer::RpcServer(coro::EventLoop& loop) : loop_(loop) {}
RpcServer::~RpcServer() = default;

// 监听并查询实际绑定端口（port=0 时由内核分配）
bool RpcServer::Start(std::string_view host, std::uint16_t port, bool reuse_port) {
    std::string h(host);
    if (!listener_.open(h.c_str(), port, reuse_port)) return false;
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(listener_.fd(), reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
        if (addr.ss_family == AF_INET) {
            bound_port_ = ntohs(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            bound_port_ = ntohs(reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
        }
    }
    if (bound_port_ == 0) bound_port_ = port;
    return true;
}

void RpcServer::Register(std::string service, std::string method, MethodHandler mh) {
    handlers_[service + "/" + method] = std::move(mh);
}

const MethodHandler* RpcServer::FindHandler(const std::string& service,
                                            const std::string& method) const {
    auto it = handlers_.find(service + "/" + method);
    return it == handlers_.end() ? nullptr : &it->second;
}

// accept 循环（webcpp-engine/server/server.cpp 同款模式）：
// accept 失败 sleep_for(10) 重试，成功 spawn RunConn 到 loop
coro::Task<void> RpcServer::Serve() {
    while (!stopped_) {
        net::TcpStream tcp;
        net::IoResult r = co_await listener_.accept(tcp);
        if (!r.ok()) {
            if (stopped_) break;
            co_await coro::sleep_for(10);
            continue;
        }
        if (stopped_) {
            tcp.close();
            break;
        }
        auto conn = std::make_shared<RpcServerConnection>(this, std::move(tcp));
        coro::Task<void> h = RunConn(conn);
        loop_.post(h.handle());
    }
    co_return;
}

}  // namespace rpc
