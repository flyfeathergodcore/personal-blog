#pragma once
#include "handler/request_handler.hpp"
#include "server/session_base.hpp"
#include "protocol/http2/stream_context.hpp"
#include "server/ws_connection_h2.hpp"
#include "protocol/http2/parser/BFL.hpp"
#include "protocol/http2/parser/HPACK.hpp"
#include "protocol/http2/parser/h2_frame_encoder.hpp"
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
    // 构造函数：保存 TLS 流、路由/中间件引用，初始化区域与本地 SETTINGS
    // 参数：stream - TLS 连接流；router - 路由表；middleware - 中间件管理器；region_pool - 请求区域池（可空）
    H2Session(net::TlsStream stream,
              Router& router,
              MiddlewareManager& middleware,
              RegionPool* region_pool);
    // 析构函数：默认实现
    ~H2Session() override;

    // 主协程：HTTP/2 会话生命周期入口（preface→读→解析→分派→处理→刷输出）
    // 参数：无
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
    H2FrameEncoder frame_enc_{output_};
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
    // 按帧类型分派处理
    // 参数：hdr - 帧头；payload - 帧负载指针
    void ProcessFrame(const H2FrameHeader& hdr, const uint8_t* payload);

    // 处理 SETTINGS：应用对端参数并回 ACK
    // 参数：hdr - 帧头；payload - 帧负载
    void OnSettings(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 HEADERS：登记流并解码 HPACK 块
    // 参数：hdr - 帧头；payload - 帧负载
    void OnHeaders(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 DATA：累积请求体/WS 数据并做流控
    // 参数：hdr - 帧头；payload - 帧负载
    void OnData(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 RST_STREAM：关闭流
    // 参数：hdr - 帧头；payload - 帧负载
    void OnRstStream(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 PING：回 PING ACK
    // 参数：hdr - 帧头；payload - 帧负载
    void OnPing(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 GOAWAY：标记对端关闭
    // 参数：hdr - 帧头；payload - 帧负载
    void OnGoAway(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 WINDOW_UPDATE：MVP 仅记录
    // 参数：hdr - 帧头；payload - 帧负载
    void OnWindowUpdate(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 PRIORITY：忽略
    // 参数：hdr - 帧头；payload - 帧负载
    void OnPriority(const H2FrameHeader& hdr, const uint8_t* payload);
    // 处理 CONTINUATION：累积并解码 HPACK 块
    // 参数：hdr - 帧头；payload - 帧负载
    void OnContinuation(const H2FrameHeader& hdr, const uint8_t* payload);

    // ── Output helpers ──
    // 追加 HEADERS 帧
    // 参数：sid - 流 ID；hpack - 已编码 HPACK 块；end_headers - 是否 END_HEADERS
    void WriteHeaders(int32_t sid, const std::vector<uint8_t>& hpack, bool end_headers);
    // 追加 DATA 帧（按帧长上限分帧）
    // 参数：sid - 流 ID；data - 负载；len - 长度；end_stream - 是否 END_STREAM
    void WriteData(int32_t sid, const uint8_t* data, size_t len, bool end_stream);
    // 追加 RST_STREAM 帧
    // 参数：sid - 流 ID；err - 错误码
    void WriteRstStream(int32_t sid, H2Error err);
    // 追加 GOAWAY 帧
    // 参数：last_sid - 最后处理流 ID；err - 错误码
    void WriteGoAway(int32_t last_sid, H2Error err);
    // 追加 WINDOW_UPDATE 帧
    // 参数：sid - 流 ID（0 连接级）；increment - 窗口增量
    void WriteWindowUpdate(int32_t sid, uint32_t increment);
    // 追加 PING ACK 帧
    // 参数：ping - 回显数据
    void WritePingAck(const H2Ping& ping);
    // 追加 SETTINGS ACK 帧
    // 参数：无
    void WriteSettingsAck();

    /// Encode response headers and write HEADERS frame.
    void WriteResponseHeaders(int32_t sid, const Response& resp);

    /// 唤醒挂起等待中的 WS 协程（数据/关闭到达时由推送侧调用）。
    void WakeWsStream(H2StreamContext& ctx);

    // ── I/O ──
    // 把 output_ 缓冲写入 socket
    // 参数：无；返回：写入成功与否
    coro::Task<bool> FlushOutput();
    // 顺序处理流待处理队列
    // 参数：无
    coro::Task<void> ProcessPending();

    // ── Stream handler ──
    // 处理单条 HTTP/2 请求流（中间件→路由→响应，支持 SSE/WS）
    // 参数：stream_id - 待处理的流 ID
    coro::Task<void> HandleStream(int32_t stream_id);
    // 独立并发运行的 WS 处理器协程，结束后清理流状态
    // 参数：h2self - 会话自身；stream_id - WS 流 ID；conn - H2 WS 连接；ws_handler - WS 处理器
    // WS 处理器协程（原为立即调用 lambda 协程，闭包悬垂 → 提为静态成员，
    // 参数按值进帧，h2self/conn 为 shared_ptr 拷贝，挂起期间始终存活）
    static coro::Task<void> RunWsHandler(
        std::shared_ptr<H2Session> h2self, int32_t stream_id,
        std::shared_ptr<H2WsConnection> conn, RequestHandler* ws_handler);
};
