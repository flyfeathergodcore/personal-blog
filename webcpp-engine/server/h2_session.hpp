#pragma once
#include "handler/request_handler.hpp"
#include "server/session_base.hpp"
#include "protocol/http2/stream_context.hpp"
#include "server/ws_connection_h2.hpp"
#include "protocol/http2/parser/BFL.hpp"
#include "protocol/http2/parser/HPACK.hpp"
#include "protocol/http2/parser/stream_manager.hpp"
#include "protocol/http2/parser/flow_control.hpp"
#include "net/tls_stream.h"
#include "coro/task.h"
#include "coro/event_loop.h"
#include <unordered_map>
#include <vector>
#include <array>
#include <memory>

class RegionPool;

// ── H2Session ──
//
// HTTP/2 会话（RFC 7540，TLS/ALPN），跑在 coro/net 协程原语之上（Task 10 移植）。
// 移除了 asio：stream_ 用 net::TlsStream；exec_ 换成 loop_（worker 事件循环引用）；
// 所有 async_* 换成 coro 读写；WS 并发用 net::spawn(loop_)。
//
// HTTP/2 session over TLS (h2).  Uses our custom H2 stack:
//   BFL          — frame encoding/decoding
//   HPACK        — header compression
//   StreamManager — stream lifecycle + pending queue
//   FlowControl  — connection/stream window management
//
// Stream handling is SEQUENTIAL for normal HTTP requests: when a
// complete request is received, the stream ID is added to a pending
// queue and the main loop drains it one at a time via HandleStream.
//
// WebSocket (RFC 8441 Extended CONNECT) streams are handled
// concurrently: HandleStream spawns the WS handler via net::spawn and
// the main loop continues processing other streams + reading data.
//
class H2Session : public SessionBase {
public:
    H2Session(net::TlsStream stream,
              Router& router,
              MiddlewareManager& middleware,
              RegionPool* region_pool);
    ~H2Session() override;

    coro::Task<void> Start() override;

private:
    friend class H2StreamSink;
    // ── Core ──
    net::TlsStream stream_;
    coro::EventLoop& loop_;   // 构造时取自 coro::EventLoop::current()（worker loop）

    // ── Custom H2 modules ──
    HpackDecoder    hpack_decoder_;
    HpackEncoder    hpack_encoder_;
    H2StreamManager stream_mgr_;
    H2FlowControl   flow_control_;

    // ── Per-stream HTTP context ──
    std::unordered_map<int32_t, H2StreamContext> streams_;

    // ── I/O buffers ──
    static constexpr size_t kReadBufSize = 65536;
    std::array<uint8_t, kReadBufSize> read_buf_;
    size_t read_buf_used_ = 0;
    std::vector<uint8_t> output_;
    bool writing_ = false;

    // ── Local settings (advertised to peer) ──
    H2Settings local_settings_;

    // ── Peer settings (from peer's SETTINGS) ──
    uint32_t peer_max_concurrent_ = 0;     // 0 = unlimited
    uint32_t peer_initial_window_ = 65535;
    uint32_t peer_max_frame_size_  = 16384;

    // ── State ──
    bool goaway_received_ = false;
    bool goaway_sent_ = false;

    // ── CONTINUATION reassembly ──
    int32_t  continuation_stream_id_ = 0;
    std::vector<uint8_t> continuation_block_;

    // ── Frame dispatch ──
    void ProcessFrame(const H2FrameHeader& hdr, const uint8_t* payload);

    void OnSettings(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnHeaders(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnData(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnRstStream(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnPing(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnGoAway(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnWindowUpdate(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnPriority(const H2FrameHeader& hdr, const uint8_t* payload);
    void OnContinuation(const H2FrameHeader& hdr, const uint8_t* payload);

    // ── Output helpers ──
    void WriteHeaders(int32_t sid, const std::vector<uint8_t>& hpack, bool end_headers);
    void WriteData(int32_t sid, const uint8_t* data, size_t len, bool end_stream);
    void WriteRstStream(int32_t sid, H2Error err);
    void WriteGoAway(int32_t last_sid, H2Error err);
    void WriteWindowUpdate(int32_t sid, uint32_t increment);
    void WritePingAck(const H2Ping& ping);
    void WriteSettingsAck();

    /// Encode response headers and write HEADERS frame.
    void WriteResponseHeaders(int32_t sid, const Response& resp);

    /// 唤醒挂起等待中的 WS 协程（数据/关闭到达时由推送侧调用）。
    void WakeWsStream(H2StreamContext& ctx);

    // ── I/O ──
    coro::Task<bool> FlushOutput();
    coro::Task<void> ProcessPending();

    // ── Stream handler ──
    coro::Task<void> HandleStream(int32_t stream_id);
};
