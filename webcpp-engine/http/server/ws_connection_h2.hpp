#pragma once
#include "http/server/ws_connection.hpp"
#include "http/server/h2_stream_writer.hpp"
#include "http/protocol/http2/stream_context.hpp"
#include "coro/task.h"
#include "coro/event_loop.h"
#include <deque>
#include <memory>
#include <coroutine>

// ═══════════════════════════════════════════════════════════════════
// H2WsConnection — WebSocket connection over H2 (RFC 8441)
//
// Implements WsConnectionBase for H2 Extended CONNECT streams.
//   - Read()  dequeues from H2StreamContext::ws_data_queue_
//   - Send()/Close() route all DATA through H2StreamWriter
//   - writer handles flow-control, frame encoding, and flushing coordination
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
    // 构造函数：绑定流写入门面、流上下文与事件循环，注册唤醒引用
    // 参数：writer - 流级写入门面；ctx - 流上下文；loop - 事件循环
    H2WsConnection(H2StreamWriter writer, H2StreamContext& ctx,
                   coro::EventLoop& loop)
        : writer_(std::move(writer))
        , ctx_(ctx)
        , loop_(loop)
    {
        ctx_.ws_wakeup_.loop = &loop_;
    }

    // 析构函数：注销唤醒引用，防止残留定时器引用即将销毁的等待帧
    ~H2WsConnection() override {
        ctx_.ws_wakeup_ = H2WsWakeup{};
    }

    // 禁止拷贝（持有会话输出缓冲与流上下文的引用）
    H2WsConnection(const H2WsConnection&) = delete;
    H2WsConnection& operator=(const H2WsConnection&) = delete;

    // ── 挂起等待 helper ──
    // 注册可取消定时器后挂起；定时器到期（100ms 轮询）或推送侧 cancel+post 时恢复。
    struct WakeAwaiter {
        H2WsWakeup& wakeup;
        std::coroutine_handle<> h;

        // 协程 awaiter：始终需要挂起
        // 参数：无；返回：false（需要挂起）
        bool await_ready() noexcept { return false; }
        // 挂起前注册可取消定时器（以协程句柄地址编码 timer_id，推送侧可反解恢复）
        // 参数：parent - 挂起中的协程句柄
        void await_suspend(std::coroutine_handle<> parent) noexcept {
            h = parent;
            wakeup.timer_id = reinterpret_cast<std::size_t>(h.address());
            wakeup.loop->wait_timer_cancelable(kWakeupPollMs, h, wakeup.timer_id);
        }
        // 恢复时的返回值
        // 参数：无
        void await_resume() noexcept {}
    };

    // ── WsConnectionBase ──

    // 从流队列读取一帧：有数据立即返回；流关闭返回空帧；否则挂起等待（推送侧可即时唤醒）
    // 参数：无；返回：WsFrame 帧对象
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

    // 发送一帧：编码为 H2 DATA 帧写入会话输出缓冲并刷新（fin 表示 END_STREAM）
    // 参数：opcode - 忽略（H2 无 WS opcode）；payload - 数据；fin - 是否结束流
    coro::Task<void> Send(WsOpcode opcode, std::string payload,
                          bool fin = true) override
    {
        if (closed_ || ctx_.ws_closed_)
            co_return;

        (void)opcode;  // H2 DATA 帧携带原始应用数据，无 WS opcode

        if (!(co_await writer_.Write(payload))) {
            closed_ = true;
            co_return;
        }
        if (fin) {
            co_await writer_.End();
            ctx_.ws_closed_ = true;
            closed_ = true;
        }
        co_return;
    }

    // 发起关闭：发送带关闭码的 DATA 帧（END_STREAM）并刷新
    // 参数：code - 关闭状态码；reason - 关闭原因
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

        co_await writer_.Write(payload);
        co_await writer_.End();
        co_return;
    }

    // 标记连接关闭并注销唤醒引用（由会话在 WS 协程结束时调用）
    // 参数：无
    void MarkClosed() {
        closed_ = true;
        ctx_.ws_closed_ = true;
        // 注销唤醒引用，防止残留定时器引用即将销毁的等待帧。
        ctx_.ws_wakeup_ = H2WsWakeup{};
    }

    /// Whether the connection is marked closed.
    bool IsClosed() const { return closed_; }

    /// End the HTTP/2 stream after a handler returns without closing it.
    coro::Task<void> Finish() {
        if (!closed_) {
            co_await writer_.End();
            closed_ = true;
        }
        co_return;
    }

private:
    static constexpr int64_t kWakeupPollMs = 100;

    H2StreamWriter writer_;
    H2StreamContext& ctx_;
    coro::EventLoop& loop_;
};
