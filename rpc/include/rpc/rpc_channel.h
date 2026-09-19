// 客户端单连接多路复用通道。
// Open 时在事件循环上 post 唯一读协程 ReadLoop；所有 UnaryCall 共享同一连接，
// 按 call_id 分派响应（多路复用）。写出口统一经 WriteLock 串行化，绝不裸 write_all。
//
// 生命周期：ReadLoop 帧引用本对象，调用方须保证本对象存活到 read_done()
// 为 true（Close 后轮询该标志）再析构。
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "coro/task.h"
#include "net/tcp_stream.h"
#include "rpc/error.h"
#include "rpc/frame.h"
#include "rpc/write_lock.h"

namespace rpc {

class RpcClientStream;  // 前向声明（完整定义在 rpc_stream.h，OpenStream 定义处包含）
class RpcStreamReader;  // 前向声明（streams_ 裸指针类型）

class RpcChannel {
public:
    RpcChannel() = default;
    ~RpcChannel();
    RpcChannel(const RpcChannel&) = delete;             // 禁止拷贝
    RpcChannel& operator=(const RpcChannel&) = delete;  // 禁止拷贝赋值

    // 连接并启动读循环；成功返回 true（失败/超时返回 false）
    // 参数：host - 主机名/IP；port - 端口；timeout_ms - 连接超时
    coro::Task<bool> Open(std::string_view host, std::uint16_t port, int64_t timeout_ms);

    // 一元调用：发送 UNARY_REQUEST 帧并等待 UNARY_RESPONSE。
    // 成功返回响应体；超时/断连/服务端错误抛 RpcException
    // 参数：service - "pkg.Service"；method - "SayHello"；payload - 请求体；timeout_ms - 调用超时
    coro::Task<std::string> UnaryCall(std::string_view service, std::string_view method,
                                      std::string_view payload, int64_t timeout_ms);

    // 开启双向流：发送 STREAM_INIT 并注册读端；返回流对象（成功返回后，
    // 首个 Read() 若遇服务端拒绝会收到 Done{NotFound} 事件而非抛异常）。
    // 流对象需保持到 Read() 返回 Done 再析构（生命周期约定见 rpc_stream.h）。
    // 参数：service - "pkg.Service"；method - "SayHello"；timeout_ms - 写 INIT 帧超时
    coro::Task<std::unique_ptr<RpcClientStream>> OpenStream(std::string_view service,
                                                            std::string_view method,
                                                            int64_t timeout_ms);

    // [内部] 经 WriteLock 串行化写一帧：所有写出的唯一出口（一元/流共用）。
    // 参数：f - 待写帧（按值接收并 move 进协程帧：协程跨挂起点使用，禁止引用
    //       调用方临时对象——调用方帧结束即悬垂，流 sender lambda 曾因此崩溃）；timeout_ms - 写超时
    coro::Task<bool> SendFrameLocked(RpcFrame f, int64_t timeout_ms = -1);

    // 关闭连接（幂等）：读循环将因 EOF 退出，未完成调用以 Closed 唤醒
    void Close();

    // 读循环是否已退出（Close 后轮询此标志，确认可安全析构）
    bool read_done() const { return read_done_.load(); }

private:
    // 一个在途调用：结果 + 等待句柄 + 可取消超时定时器 id
    struct PendingCall {
        std::coroutine_handle<> h;
        std::string result;
        std::string err;
        RpcCode code = RpcCode::Ok;
        std::uint64_t timer_id = 0;  // 0 = 未注册定时器（无限等待）
        bool done = false;           // 响应已到达（ReadLoop/FailAll 置位）
    };
    // 等待响应/超时的 awaiter：响应先到 cancel_timer 作废；超时先到同步 erase
    struct AwaitResponse {
        RpcChannel* ch;
        std::uint64_t call_id;
        std::shared_ptr<PendingCall> pc;
        int64_t timeout_ms;
        bool await_ready() noexcept { return pc->done; }
        void await_suspend(std::coroutine_handle<> h);
        std::string await_resume();
    };

    coro::Task<void> ReadLoop();  // 连接唯一读协程
    void FailAll();               // 断连：以 Closed 唤醒全部在途调用
    void FailStreams();           // 断连：以 Done{Closed} 唤醒全部流读端并清空

    net::TcpStream stream_;
    FrameReader reader_;  // 可恢复帧读取器：轮询超时不打断帧完整性
    WriteLock write_lock_;
    std::uint64_t next_call_id_ = 1;  // call_id 0 保留、单调递增
    std::map<std::uint64_t, std::shared_ptr<PendingCall>> pending_;
    std::map<std::uint64_t, RpcStreamReader*> streams_;  // 流 call_id → 读端（裸指针，流对象生命周期约定见 rpc_stream.h）
    bool closed_ = false;
    std::atomic<bool> read_done_{false};
};

}  // namespace rpc
