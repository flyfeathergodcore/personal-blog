#pragma once
#include "server/ws_connection.hpp"
#include "protocol/http2/stream_context.hpp"
#include "protocol/http2/parser/BFL.hpp"
#include "coro/task.h"
#include "coro/event_loop.h"
#include <deque>
#include <memory>
#include <functional>
#include <cstring>
#include <coroutine>

// ═══════════════════════════════════════════════════════════════════
// H2WsConnection — WebSocket connection over H2 (RFC 8441)
//
// Implements WsConnectionBase for H2 Extended CONNECT streams.
//   - Read()  dequeues from H2StreamContext::ws_data_queue_
//   - Send()  builds a DATA frame directly into the session output buffer
//   - Close() sends END_STREAM via DATA frame with close code
//
// Frame building: uses BFL functions (EncodeFrameHeader) and pushes
// raw bytes into the session's output buffer, then calls the flusher.
//
// 唤醒机制（coro 版）：
//   等待侧（Read 内的 WakeAwaiter）调用 loop_->wait_timer_cancelable(...) 挂起，
//   100ms 轮询兜底；session 推送侧（OnData/RstStream）在数据/关闭到达时
//   loop_->cancel_timer(id) + post(句柄) 即时唤醒，消除轮询延迟。
//   timer_id 用等待协程句柄地址编码——等待帧地址唯一，推送侧可反解句柄 resume。
//   等待侧与 session 主循环同跑一个 worker 事件循环（单线程），无需加锁。
// ═══════════════════════════════════════════════════════════════════

class H2WsConnection : public WsConnectionBase,
                       public std::enable_shared_from_this<H2WsConnection> {
public:
    using Flusher = std::function<coro::Task<bool>()>;

    H2WsConnection(std::vector<uint8_t>& output, int32_t stream_id,
                   H2StreamContext& ctx, coro::EventLoop& loop,
                   Flusher flusher)
        : output_(output)
        , stream_id_(stream_id)
        , ctx_(ctx)
        , loop_(loop)
        , flusher_(std::move(flusher))
    {
        ctx_.ws_wakeup_.loop = &loop_;
    }

    ~H2WsConnection() override {
        ctx_.ws_wakeup_ = H2WsWakeup{};
    }

    H2WsConnection(const H2WsConnection&) = delete;
    H2WsConnection& operator=(const H2WsConnection&) = delete;

    // ── 挂起等待 helper ──
    // 注册可取消定时器后挂起；定时器到期（100ms 轮询）或推送侧 cancel+post 时恢复。
    struct WakeAwaiter {
        H2WsWakeup& wakeup;
        std::coroutine_handle<> h;

        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> parent) noexcept {
            h = parent;
            wakeup.timer_id = reinterpret_cast<std::size_t>(h.address());
            wakeup.loop->wait_timer_cancelable(kWakeupPollMs, h, wakeup.timer_id);
        }
        void await_resume() noexcept {}
    };

    // ── WsConnectionBase ──

    coro::Task<WsFrame> Read() override
    {
        while (!closed_ && !ctx_.ws_closed_)
        {
            if (!ctx_.ws_data_queue_.empty()) {
                auto data = std::move(ctx_.ws_data_queue_.front());
                ctx_.ws_data_queue_.pop_front();
                co_return WsFrame{true, WsOpcode::Binary, std::move(data)};
            }

            if (ctx_.stream_closed_) {
                closed_ = true;
                co_return WsFrame{};
            }

            // 挂起等待——定时器到期自动恢复；数据到达时由 session 推送侧
            // cancel_timer + post 提前恢复（消除 100ms 轮询延迟）。
            co_await WakeAwaiter{ctx_.ws_wakeup_};
        }

        closed_ = true;
        co_return WsFrame{};
    }

    coro::Task<void> Send(WsOpcode opcode, std::string payload,
                          bool fin = true) override
    {
        if (closed_ || ctx_.ws_closed_)
            co_return;

        (void)opcode;  // H2 DATA 帧携带原始应用数据，无 WS opcode

        uint8_t flags = fin ? H2Flags::END_STREAM : 0;
        size_t pos = output_.size();
        output_.resize(pos + kFrameHeaderSize + payload.size());
        EncodeFrameHeader(output_.data() + pos,
            {static_cast<uint32_t>(payload.size()), H2FrameType::DATA, flags, static_cast<uint32_t>(stream_id_)});
        if (!payload.empty())
            std::memcpy(output_.data() + pos + kFrameHeaderSize, payload.data(), payload.size());

        if (fin) {
            ctx_.ws_closed_ = true;
            closed_ = true;
        }

        if (flusher_)
            co_await flusher_();
        co_return;
    }

    coro::Task<void> Close(uint16_t code = 1000,
                           std::string_view reason = {}) override
    {
        if (closed_ || ctx_.ws_closed_) co_return;
        closed_ = true;
        ctx_.ws_closed_ = true;

        std::string payload;
        payload.push_back(static_cast<char>(code >> 8));
        payload.push_back(static_cast<char>(code & 0xFF));
        payload.append(reason);

        size_t pos = output_.size();
        output_.resize(pos + kFrameHeaderSize + payload.size());
        EncodeFrameHeader(output_.data() + pos,
            {static_cast<uint32_t>(payload.size()), H2FrameType::DATA,
             H2Flags::END_STREAM, static_cast<uint32_t>(stream_id_)});
        if (!payload.empty())
            std::memcpy(output_.data() + pos + kFrameHeaderSize, payload.data(), payload.size());

        if (flusher_)
            co_await flusher_();
        co_return;
    }

    void MarkClosed() {
        closed_ = true;
        ctx_.ws_closed_ = true;
        // 注销唤醒引用，防止残留定时器引用即将销毁的等待帧。
        ctx_.ws_wakeup_ = H2WsWakeup{};
    }

    /// Whether the connection is marked closed.
    bool IsClosed() const { return closed_; }

private:
    static constexpr int64_t kWakeupPollMs = 100;

    std::vector<uint8_t>& output_;
    int32_t stream_id_;
    H2StreamContext& ctx_;
    coro::EventLoop& loop_;
    Flusher flusher_;
};
