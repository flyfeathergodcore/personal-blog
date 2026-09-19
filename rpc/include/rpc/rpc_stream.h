// 双向流框架层原语：服务端/客户端共用。
// 协议：STREAM_INIT 开流 → STREAM_DATA 双向传消息 → STREAM_DONE 表达"该方向结束/半关闭"。
//   - 读端 RpcStreamReader：单消费者信箱——读循环 Post 投递帧，消费方 Read() 挂起等待；
//   - 写端 RpcStreamWriter：经连接 WriteLock 串行化发帧，Finish() 半关闭（之后不再 Write）；
//   - 双端都发完 DONE 才回收 call_id（服务端 StreamContext / 客户端 channel 的 streams_）。
//
// 生命周期约定：客户端流对象 RpcClientStream 的 reader 注册在所属 channel 的
// streams_（裸指针）。调用方必须保持流对象存活到对端 DONE（Read() 返回 Done 事件）
// 且 channel 存活到流结束之后再析构，否则帧到达时读端悬垂。
#pragma once

#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

#include "coro/task.h"
#include "rpc/error.h"
#include "rpc_meta.pb.h"

namespace rpc {

class RpcChannel;  // 客户端通道（前向声明：OpenStream 定义在 rpc_channel.cpp）

// 流事件：读取端 Read() 的返回值
enum class StreamEventKind { Data, Done, Timeout };

// 一条流事件：Data 携带一条消息；Done 携带对端结束状态；Timeout 表示读超时
struct StreamEvent {
    StreamEventKind kind = StreamEventKind::Timeout;
    RpcCode code = RpcCode::Ok;  // Done 时携带对端状态
    std::string payload;         // Data 时携带一条消息
    std::string err;             // Done 时携带错误消息
};

// 流读取端：单消费者信箱。读循环（客户端 RpcChannel::ReadLoop / 服务端
// RpcServerConnection::Serve）把收到的 STREAM_DATA/DONE Post 进来；消费方
// Read() 挂起等待。单线程事件循环下无锁。同一时刻只允许一个协程 Read()。
class RpcStreamReader {
public:
    RpcStreamReader() = default;
    RpcStreamReader(const RpcStreamReader&) = delete;             // 禁止拷贝
    RpcStreamReader& operator=(const RpcStreamReader&) = delete;  // 禁止拷贝赋值

    // 取一条事件：有数据立即返回 Data；收到过 DONE 返回 Done；否则挂起等待。
    // 超时（timeout_ms >= 0）返回 Timeout 事件，已到达的事件不丢失（可续读）。
    // 参数：timeout_ms - 等待上限（毫秒，<0 无限）
    coro::Task<StreamEvent> Read(int64_t timeout_ms = -1);

    // 读循环投递一条事件：唤醒阻塞在 Read() 的等待者（若有），否则入队缓冲
    void Post(StreamEvent ev);

    // 是否已收到对端 DONE（流结束）
    bool done() const { return done_; }

private:
    friend struct StreamReadAwaiter;
    std::deque<StreamEvent> queue_;    // 先于 Read() 到达的消息缓冲
    std::coroutine_handle<> waiter_{}; // 阻塞在 Read() 的协程（空 = 无）
    std::size_t waiter_timer_id_ = 0;  // 等待者的可取消超时定时器 id
    bool done_ = false;                // 已收到 DONE / 流已结束
};

// 流写出端：绑定连接发帧回调，Write 发 STREAM_DATA，Finish 发 STREAM_DONE。
// 同一连接的写出口统一经 WriteLock 串行化（发帧回调内部保证）。
class RpcStreamWriter {
public:
    RpcStreamWriter() = default;

    // 绑定：call_id + 发帧回调（把一帧写到本流所属连接，含写超时）
    // 参数：call_id - 流调用 id；sender - 发帧回调（内部经 WriteLock 串行化）
    void SetSender(std::uint64_t call_id, std::function<coro::Task<bool>(RpcFrame, int64_t)> sender);

    // 发一条业务消息（STREAM_DATA）；已半关闭或未绑定返回 false
    // 参数：payload - 消息字节；timeout_ms - 写超时（毫秒，<0 无限）
    coro::Task<bool> Write(std::string_view payload, int64_t timeout_ms = -1);

    // 半关闭/结束：发 STREAM_DONE（幂等，只发一次）；之后 Write 返回 false
    // 参数：code - 结束状态；err - 错误消息；timeout_ms - 写超时
    coro::Task<bool> Finish(RpcCode code = RpcCode::Ok, std::string_view err = "",
                            int64_t timeout_ms = -1);

    // 是否已发过 STREAM_DONE
    bool write_done() const { return write_done_; }

private:
    std::uint64_t call_id_ = 0;
    std::function<coro::Task<bool>(RpcFrame, int64_t)> sender_;
    bool write_done_ = false;
};

// 服务端一条流的上下文（RpcServerConnection::streams_ 持有，shared_ptr 保活）
struct StreamContext {
    RpcStreamReader reader;  // 客户端→服务端（handler Read 的输入）
    RpcStreamWriter writer;  // 服务端→客户端（handler Write/Finish 的输出）
    bool client_done = false;  // 收到客户端 STREAM_DONE
    bool server_done = false;  // 服务端已发 STREAM_DONE
};

// 客户端流对象：绑定到 RpcChannel，读写该流的原始字节。
// 生命周期约定见文件头注释：保持本对象存活到对端 DONE 再析构。
class RpcClientStream {
public:
    // 构造：绑定 channel 与流 call_id（由 RpcChannel::OpenStream 创建）
    RpcClientStream(RpcChannel* ch, std::uint64_t call_id);
    ~RpcClientStream();
    RpcClientStream(const RpcClientStream&) = delete;             // 禁止拷贝
    RpcClientStream& operator=(const RpcClientStream&) = delete;  // 禁止拷贝赋值
    RpcClientStream(RpcClientStream&&) = delete;                  // 禁止移动（reader 已在 channel 注册）
    RpcClientStream& operator=(RpcClientStream&&) = delete;

    // 读取端/写出端（读写原始字节；typed 包装由 protoc 插件 codegen 生成）
    RpcStreamReader& reader() { return reader_; }
    RpcStreamWriter& writer() { return writer_; }
    std::uint64_t call_id() const { return call_id_; }

    // 便捷：发一条消息
    // 参数：payload - 消息字节；timeout_ms - 写超时（毫秒，<0 无限）
    coro::Task<bool> Write(std::string_view payload, int64_t timeout_ms = -1) {
        return writer_.Write(payload, timeout_ms);
    }
    // 便捷：读一条事件（Data/Done/Timeout）
    // 参数：timeout_ms - 等待上限（毫秒，<0 无限）
    coro::Task<StreamEvent> Read(int64_t timeout_ms = -1) { return reader_.Read(timeout_ms); }
    // 便捷：半关闭（发 STREAM_DONE，之后不能 Write，仍可 Read）
    // 参数：timeout_ms - 写超时（毫秒，<0 无限）
    coro::Task<bool> CloseWrite(int64_t timeout_ms = -1) {
        return writer_.Finish(RpcCode::Ok, "", timeout_ms);
    }

private:
    RpcChannel* ch_;
    std::uint64_t call_id_;
    RpcStreamReader reader_;
    RpcStreamWriter writer_;
};

}  // namespace rpc
