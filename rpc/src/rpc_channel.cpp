// RpcChannel 实现：连接/读循环/一元调用/双向流/超时/断连唤醒
#include "rpc/rpc_channel.h"

#include <sys/socket.h>
#include <utility>

#include "coro/event_loop.h"
#include "net/resolver.h"
#include "rpc/rpc_stream.h"
#include "rpc/timer_id.h"

namespace rpc {

// 读循环轮询间隔：有限超时让"fd 已关闭"也能被感知——macOS kqueue 不会唤醒
// 已关闭 fd 上已注册的读等待，纯无限超时会让 ReadLoop 永久挂起。每次超时回到
// 循环顶检查 closed_，同时 FrameReader 保留帧进度，轮询不打断帧完整性。
inline constexpr int64_t kReadPollMs = 100;

RpcChannel::~RpcChannel() = default;

// 连接并启动唯一读协程：net::connect 移动接管 TcpStream，随后 post ReadLoop
coro::Task<bool> RpcChannel::Open(std::string_view host, std::uint16_t port,
                                  int64_t timeout_ms) {
    if (closed_) co_return false;
    auto s = co_await net::connect(host, port, timeout_ms);
    if (!s) co_return false;
    stream_ = std::move(*s);
    closed_ = false;
    read_done_.store(false);
    reader_.Reset();  // 清空旧连接可能残留的部分帧
    coro::EventLoop::current().post(ReadLoop().handle());
    co_return true;
}

// 一元调用：登记在途调用 → 经 WriteLock 串行写请求帧 → 等响应/超时
coro::Task<std::string> RpcChannel::UnaryCall(std::string_view service,
                                              std::string_view method,
                                              std::string_view payload,
                                              int64_t timeout_ms) {
    if (closed_ || !stream_.is_open()) {
        throw RpcException(RpcCode::Closed, "channel not open");
    }
    const std::uint64_t cid = next_call_id_++;
    auto pc = std::make_shared<PendingCall>();
    pending_[cid] = pc;

    RpcFrame req;
    req.set_kind(RpcKind::UNARY_REQUEST);
    req.set_call_id(cid);
    req.set_service_name(service.data(), service.size());
    req.set_method_name(method.data(), method.size());
    req.set_payload(payload.data(), payload.size());

    const bool wrote = co_await SendFrameLocked(std::move(req), timeout_ms);
    if (!wrote) {
        pending_.erase(cid);
        throw RpcException(RpcCode::Closed, "request write failed");
    }
    co_return co_await AwaitResponse{this, cid, pc, timeout_ms};
}

// 经 WriteLock 串行化写一帧：所有写出的唯一出口（UnaryCall 请求/响应、流帧共用）。
// 按值接收帧并 move 进协程帧：协程可能挂起在 Acquire/WriteFrame 上，期间调用方
// 的临时对象（流 sender lambda 的按值参数）已析构，引用即悬垂——必须持有副本。
coro::Task<bool> RpcChannel::SendFrameLocked(RpcFrame f, int64_t timeout_ms) {
    if (!stream_.is_open()) co_return false;
    if (!co_await write_lock_.Acquire(-1)) co_return false;
    const bool ok = co_await WriteFrame(stream_, f, timeout_ms);
    write_lock_.Release();  // 无论成败都释放：写失败抛异常前必须归还锁，否则锁泄漏死锁
    co_return ok;
}

// 开启双向流：先注册读端（STREAM_INIT 之后的服务端帧不会漏），再发 STREAM_INIT
coro::Task<std::unique_ptr<RpcClientStream>> RpcChannel::OpenStream(std::string_view service,
                                                                    std::string_view method,
                                                                    int64_t timeout_ms) {
    if (closed_ || !stream_.is_open()) {
        throw RpcException(RpcCode::Closed, "channel not open");
    }
    const std::uint64_t cid = next_call_id_++;
    auto st = std::make_unique<RpcClientStream>(this, cid);
    streams_[cid] = &st->reader();  // 先注册读端再发帧：避免服务端首帧先到而漏投递

    RpcFrame init;
    init.set_kind(RpcKind::STREAM_INIT);
    init.set_call_id(cid);
    init.set_service_name(service.data(), service.size());
    init.set_method_name(method.data(), method.size());

    if (!co_await SendFrameLocked(std::move(init), timeout_ms)) {
        streams_.erase(cid);
        throw RpcException(RpcCode::Closed, "stream init write failed");
    }
    co_return std::move(st);
}

// 挂起时注册可取消超时定时器；两步（存句柄 + 注册定时器）同步完成无竞态
void RpcChannel::AwaitResponse::await_suspend(std::coroutine_handle<> h) {
    pc->h = h;
    if (timeout_ms >= 0) {
        pc->timer_id = NextTimerId();
        coro::EventLoop::current().wait_timer_cancelable(timeout_ms, h, pc->timer_id);
    }
}

// 醒来分两类：
//  - !done：定时器到期恢复本协程（超时）→ 同步从等待表移除，晚到响应被丢弃
//  - done ：响应/断连已置位 → 按 code 返回或抛异常
std::string RpcChannel::AwaitResponse::await_resume() {
    if (!pc->done) {
        ch->pending_.erase(call_id);
        throw RpcException(RpcCode::Timeout, "rpc unary timeout");
    }
    if (pc->code != RpcCode::Ok) throw RpcException(pc->code, pc->err);
    return std::move(pc->result);
}

// 连接唯一读协程：FrameReader 轮询循环 → 按 call_id 分派响应；断连 FailAll 防帧泄漏
coro::Task<void> RpcChannel::ReadLoop() {
    for (;;) {
        if (closed_) break;  // 轮询点：Close 置位后及时退出
        RpcFrame f;
        net::IoResult r = co_await reader_.Read(stream_, &f, kReadPollMs);
        if (r.err == net::IoError::Timeout) continue;  // 空转轮询，回循环顶查 closed_
        if (!r.ok()) break;  // EOF / 断开 / 已关闭 fd
        if (f.kind() == RpcKind::STREAM_DATA) {
            auto it = streams_.find(f.call_id());
            if (it != streams_.end()) {
                it->second->Post(StreamEvent{StreamEventKind::Data, RpcCode::Ok,
                                             f.payload(), ""});
            }
            continue;
        }
        if (f.kind() == RpcKind::STREAM_DONE) {
            auto it = streams_.find(f.call_id());
            if (it != streams_.end()) {
                auto* rd = it->second;
                streams_.erase(it);  // 服务端已结束：之后该流无更多帧
                rd->Post(StreamEvent{StreamEventKind::Done, from_proto_status(f.status()),
                                     "", f.error_message()});
            }
            continue;
        }
        if (f.kind() != RpcKind::UNARY_RESPONSE) continue;  // 未知类型，丢弃
        auto it = pending_.find(f.call_id());
        if (it == pending_.end()) continue;  // 已超时/已删除，丢弃
        auto pc = it->second;
        pending_.erase(it);
        pc->done = true;
        if (f.status() == RpcStatus::ST_OK) {
            pc->code = RpcCode::Ok;
            pc->result = f.payload();
        } else {
            pc->code = from_proto_status(f.status());
            pc->err = f.error_message();
        }
        // 作废超时定时器：返回 false 表示到期路径已消费（resume 已安排），
        // 不得再手动 resume，否则双 resume。
        bool should_resume = true;
        if (pc->timer_id != 0) {
            should_resume = coro::EventLoop::current().cancel_timer(pc->timer_id);
        }
        if (should_resume) pc->h.resume();
    }
    FailAll();
    FailStreams();
    read_done_.store(true);
    co_return;
}

// 断连：以 ST_CLOSED 唤醒全部在途调用（与 ReadLoop 分派同样的双 resume 防护）
void RpcChannel::FailAll() {
    for (auto& kv : pending_) {
        auto& pc = kv.second;
        pc->done = true;
        pc->code = RpcCode::Closed;
        pc->err = "connection closed";
        bool should_resume = true;
        if (pc->timer_id != 0) {
            should_resume = coro::EventLoop::current().cancel_timer(pc->timer_id);
        }
        if (should_resume) pc->h.resume();
    }
    pending_.clear();
    closed_ = true;
}

// 断连：以 Done{Closed} 唤醒全部流读端（阻塞在流 Read() 的 handler/客户端得以退出）
void RpcChannel::FailStreams() {
    for (auto& kv : streams_) {
        kv.second->Post(StreamEvent{StreamEventKind::Done, RpcCode::Closed, "",
                                    "connection closed"});
    }
    streams_.clear();
}

// 关闭连接：置关闭标志并立即关闭 fd（shutdown 发 FIN 通知对端尽快清理）。
// ReadLoop 通过轮询超时感知 closed_ 并退出；未完成调用由 FailAll 唤醒。
// 幂等：已关闭且读循环已退出时直接返回。
void RpcChannel::Close() {
    if (closed_ && read_done_.load()) return;
    closed_ = true;
    if (stream_.is_open()) {
        ::shutdown(stream_.fd(), SHUT_RDWR);
        stream_.close();
    }
}

}  // namespace rpc
