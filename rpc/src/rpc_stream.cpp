// 流原语实现：RpcStreamReader（信箱读端）、RpcStreamWriter（写端）、RpcClientStream。
#include "rpc/rpc_stream.h"

#include <utility>

#include "coro/event_loop.h"
#include "rpc/rpc_channel.h"
#include "rpc/timer_id.h"

namespace rpc {

// ════ RpcStreamReader ════

// 等待一条事件的 awaiter：挂起时存句柄并注册可取消超时定时器；
// 恢复时清理定时器，从队列取事件（无事件且非 DONE → Timeout）
struct StreamReadAwaiter {
    RpcStreamReader* r;
    int64_t timeout_ms;
    std::size_t timer_id = 0;
    std::coroutine_handle<> h;

    bool await_ready() noexcept { return r->done_ || !r->queue_.empty(); }
    void await_suspend(std::coroutine_handle<> hh) noexcept {
        h = hh;
        r->waiter_ = h;
        if (timeout_ms >= 0) {
            // 可取消定时器：先存 waiter 再注册，两步同步无竞态
            timer_id = NextTimerId();
            r->waiter_timer_id_ = timer_id;
            coro::EventLoop::current().wait_timer_cancelable(timeout_ms, h, timer_id);
        }
    }
    StreamEvent await_resume() {
        // 清等待者登记（Post 唤醒时已清；定时器唤醒时在此清）
        if (r->waiter_ == h) {
            r->waiter_ = {};
            r->waiter_timer_id_ = 0;
        }
        if (timer_id != 0) coro::EventLoop::current().cancel_timer(timer_id);  // 作废未触发的定时器
        if (!r->queue_.empty()) {
            StreamEvent ev = std::move(r->queue_.front());
            r->queue_.pop_front();
            if (ev.kind == StreamEventKind::Done) r->done_ = true;  // 消费 DONE 后流结束
            return ev;
        }
        if (r->done_) return StreamEvent{StreamEventKind::Done, RpcCode::Closed, "", "stream closed"};
        return StreamEvent{StreamEventKind::Timeout, RpcCode::Timeout, "", "read timeout"};
    }
};

coro::Task<StreamEvent> RpcStreamReader::Read(int64_t timeout_ms) {
    co_return co_await StreamReadAwaiter{this, timeout_ms};
}

void RpcStreamReader::Post(StreamEvent ev) {
    if (done_) return;  // 流已结束，忽略迟到帧
    queue_.push_back(std::move(ev));
    if (waiter_) {
        auto w = waiter_;
        waiter_ = {};
        // 唤醒等待者：其 await_resume 会 cancel_timer 作废自身定时器；
        // 若定时器已先到期（cancel_timer 返回 false），等待者已在定时器路径被恢复过——
        // 但此处事件已入队，等待者恢复后取到的是事件而非超时，不会双重 resume。
        w.resume();
    }
}

// ════ RpcStreamWriter ════

void RpcStreamWriter::SetSender(std::uint64_t call_id,
                                std::function<coro::Task<bool>(RpcFrame, int64_t)> sender) {
    call_id_ = call_id;
    sender_ = std::move(sender);
}

coro::Task<bool> RpcStreamWriter::Write(std::string_view payload, int64_t timeout_ms) {
    if (write_done_ || !sender_) co_return false;  // 已半关闭或未绑定
    RpcFrame f;
    f.set_kind(RpcKind::STREAM_DATA);
    f.set_call_id(call_id_);
    f.set_payload(payload.data(), payload.size());
    co_return co_await sender_(std::move(f), timeout_ms);
}

coro::Task<bool> RpcStreamWriter::Finish(RpcCode code, std::string_view err, int64_t timeout_ms) {
    if (write_done_) co_return true;  // 幂等：只发一次
    write_done_ = true;
    if (!sender_) co_return true;  // 未绑定（连接已死）：视为已结束
    RpcFrame f;
    f.set_kind(RpcKind::STREAM_DONE);
    f.set_call_id(call_id_);
    f.set_status(to_proto_status(code));
    f.set_error_message(err.data(), err.size());
    co_return co_await sender_(std::move(f), timeout_ms);
}

// ════ RpcClientStream ════

RpcClientStream::RpcClientStream(RpcChannel* ch, std::uint64_t call_id)
    : ch_(ch), call_id_(call_id) {
    // 绑定写出端：发帧经所属 channel 的 WriteLock 串行化（SendFrameLocked）
    writer_.SetSender(call_id, [ch, call_id](RpcFrame f, int64_t tmo) -> coro::Task<bool> {
        return ch->SendFrameLocked(std::move(f), tmo);
    });
}

RpcClientStream::~RpcClientStream() = default;

}  // namespace rpc
