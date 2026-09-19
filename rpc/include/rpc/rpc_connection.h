// 服务端单个连接：读循环 + 一元分发。
// 连接协程由 RpcServer::Serve spawn（外层持有 shared_ptr 保活）；
// 写出口统一经 WriteLock 串行化。
// 生命周期：派生 enable_shared_from_this，Serve 分发时对每个请求取 shared_ptr
// 副本保活连接——handler 可能长于 Serve 存活（慢 handler 期间对端断开），
// 连接在全部 handler 完成前不得析构。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "coro/task.h"
#include "net/tcp_stream.h"
#include "rpc/codegen.h"
#include "rpc/rpc_stream.h"
#include "rpc/write_lock.h"
#include "rpc_meta.pb.h"

namespace rpc {

class RpcServer;

class RpcServerConnection : public std::enable_shared_from_this<RpcServerConnection> {
public:
    // 构造：绑定所属 server 与接管连接流
    RpcServerConnection(RpcServer* server, net::TcpStream stream);
    ~RpcServerConnection();
    RpcServerConnection(const RpcServerConnection&) = delete;             // 禁止拷贝
    RpcServerConnection& operator=(const RpcServerConnection&) = delete;  // 禁止拷贝赋值

    // 读循环：ReadFrame → 按 kind 分发；返回时连接结束（对端断开/错误）
    coro::Task<void> Serve();

    // 处理一个一元请求（独立协程，由 RunUnary 持 shared_ptr 保活连接）：
    // 调 handler → 序列化响应 → 经 WriteLock 写回
    // 参数按值接收帧：避免引用读循环局部变量（其会在下一轮迭代被复用）
    coro::Task<void> HandleUnary(RpcFrame req);

    // [内部] 经 WriteLock 串行化写一帧：所有写出的唯一出口（一元/流共用）。
    // 参数：f - 待写帧（按值接收并 move 进协程帧：协程跨挂起点使用，禁止引用
    //       调用方临时对象——流 sender lambda 的帧一结束引用即悬垂）；timeout_ms - 写超时
    coro::Task<bool> SendFrameLocked(RpcFrame f, int64_t timeout_ms = -1);

private:
    // 流处理协程：持 self（shared_ptr）保活连接与流上下文，调用户流 handler；
    // handler 未显式 Finish 时框架补发 STREAM_DONE{OK}；双端 DONE 后回收流上下文
    // 参数：self - 连接 shared_ptr 副本（保活）；mh - 已查到的流处理器；ctx - 流上下文；init - 开流帧
    coro::Task<void> RunServerStream(std::shared_ptr<RpcServerConnection> self,
                                     const MethodHandler* mh,
                                     std::shared_ptr<StreamContext> ctx, RpcFrame init);

    // 回收流：双端 DONE 才从 streams_ 移除（释放流上下文）
    void RecycleStream(std::uint64_t call_id);

    RpcServer* server_;
    net::TcpStream stream_;
    WriteLock write_lock_;
    // 流 call_id → 流上下文（shared_ptr 保活：服务端 handler 独立协程持引用，
    // 连接断开时 Serve 清空此表并 Post(Closed) 唤醒阻塞的 handler）
    std::map<std::uint64_t, std::shared_ptr<StreamContext>> streams_;
};

}  // namespace rpc
