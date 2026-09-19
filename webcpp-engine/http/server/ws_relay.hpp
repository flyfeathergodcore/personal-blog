#pragma once
#include "http/protocol/ws_frame.hpp"
#include "coro/task.h"
#include <atomic>

// ═══════════════════════════════════════════════════════════════════
// WsRelay — bidirectional WebSocket frame relay
//
// Relays frames between two streams (e.g., client ↔ upstream).
// One direction reads from `from` and writes to `to`, optionally
// re-masking frames (mask_to=true when relaying client→upstream).
//
// Usage:
//   auto relay_c2u = WsRelayDirectional(client_sock, upstream_sock, true, cancel);
//   auto relay_u2c = WsRelayDirectional(upstream_sock, client_sock, false, cancel);
//   co_await relay_c2u;  // wait for either direction to finish
//   cancel = true;       // cancel the other direction
// ═══════════════════════════════════════════════════════════════════

/// Relay frames from `from` to `to`.
/// When a Close frame is received, it is forwarded and the relay stops.
/// 单向中继帧：从 from 读到 to；收到 Close 帧转发后停止；cancel_flag 置位则退出
/// 参数：from - 读取侧流；to - 写入侧流；mask_to - 转发时是否重新加掩码；cancel_flag - 取消标志（跨协程）
template<typename FromStream, typename ToStream>
coro::Task<void> WsRelayDirectional(
    FromStream& from, ToStream& to,
    bool mask_to,
    std::atomic<bool>& cancel_flag)
{
    while (!cancel_flag.load(std::memory_order_relaxed))
    {
        // ReadFrame 出错/超时/半读返回 nullopt → 连接结束
        auto opt = co_await ReadFrame(from);
        if (!opt.has_value())
            break;  // read error, timeout, or connection closed

        auto frame = std::move(*opt);

        // Forward close frame
        if (frame.opcode == WsOpcode::Close)
        {
            co_await WriteFrame(to, WsOpcode::Close,
                                std::move(frame.payload), true, mask_to);
            cancel_flag.store(true, std::memory_order_relaxed);
            break;
        }

        // Re-mask if needed (relay adds masking when forwarding client→upstream)
        co_await WriteFrame(to, frame.opcode,
                            std::move(frame.payload), frame.fin, mask_to);
    }
    co_return;
}
