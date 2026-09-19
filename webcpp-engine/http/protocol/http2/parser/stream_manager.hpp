#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <deque>

// ── HTTP/2 stream states (RFC 7540 §5.1) ──
//
// For a server receiving client requests, the typical path is:
//   idle → open → half_closed_remote → closed
//
// WebSocket (RFC 8441) stays open for bidirectional DATA, then closed.
//
enum class H2StreamState : uint8_t {
    Idle,
    Open,
    HalfClosedRemote,   // client sent END_STREAM
    HalfClosedLocal,    // server sent END_STREAM / RST_STREAM
    Closed,
};

// ═══════════════════════════════════════════════════════════════
// H2StreamManager — stream lifecycle, ID validation, pending queue
//
// Owns the stream state machine and the sequential-processing queue.
// Does NOT own per-stream HTTP context data (that stays in
// H2StreamContext, managed by the Session).
// ═══════════════════════════════════════════════════════════════

class H2StreamManager {
public:
    enum class OpenResult : uint8_t { Accepted, ProtocolError, Refused };
    // 默认构造函数。
    H2StreamManager() = default;

    // ── Stream lifecycle ──

    /// 客户端发起的新流 HEADERS 到达：校验流 ID（奇数、单调递增）。
    /// 协议错误返回 false。
    OpenResult OnStreamOpen(int32_t stream_id);

    /// 客户端在 HEADERS 或 DATA 上设置 END_STREAM。
    void OnStreamEndStream(int32_t stream_id);

    /// 收到 RST_STREAM 或本端发起关闭。
    void OnStreamClose(int32_t stream_id);

    /// 移除一个已完全关闭的流。
    void RemoveStream(int32_t stream_id);

    /// 查询流的当前状态。
    H2StreamState GetState(int32_t stream_id) const;
    /// 判断流是否处于活动状态（open / half-closed）。
    bool IsActive(int32_t stream_id) const;

    // ── Pending queue ──

    /// 将已完成的请求流加入顺序处理队列。
    void Enqueue(int32_t stream_id);

    /// 弹出下一个待处理流，队列为空返回 0。
    int32_t Dequeue();

    /// 待处理流数量。
    size_t PendingCount() const { return pending_.size(); }
    /// 是否有待处理流。
    bool   HasPending()  const { return !pending_.empty(); }

    // ── Limits ──

    // 设置最大并发流数。
    void  SetMaxConcurrent(uint32_t max) { max_concurrent_ = max; }
    // 查询最大并发流数。
    uint32_t MaxConcurrent() const { return max_concurrent_; }
    // 是否允许创建新流（活动流数未达上限）。
    bool     CanCreateStream() const;

    /// 活动流总数（open + half-closed）。
    uint32_t ActiveCount() const { return active_count_; }

    /// 客户端已见的最大流 ID（用于 GOAWAY）。
    int32_t LastClientStreamId() const { return last_client_stream_id_; }

    // ── Cleanup ──

    /// 移除所有状态为 Closed 的流。
    void GcClosed();

private:
    std::unordered_map<int32_t, H2StreamState> states_;
    std::deque<int32_t> pending_;

    uint32_t max_concurrent_ = 100;
    uint32_t active_count_ = 0;
    int32_t  last_client_stream_id_ = 0;
};
