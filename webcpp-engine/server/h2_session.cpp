#include "server/h2_session.hpp"
#include "server/h2_stream_writer.hpp"
#include "server/h2_stream_processor.hpp"
#include "protocol/region_pool.hpp"
#include "server/sse_push.hpp"
#include "handler/metrics.hpp"
#include "protocol/response.hpp"
#include "net/when_all.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <utility>
#include <coroutine>
#include <openssl/ssl.h>
#include <atomic>

namespace {
std::atomic<std::size_t> g_h2_waiter_id{1};
}

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

// 构造函数：保存 TLS 流与路由/中间件引用，初始化区域与本地 SETTINGS（含 ENABLE_CONNECT_PROTOCOL）
// 参数：stream - TLS 连接流；router - 路由表；middleware - 中间件管理器；region_pool - 请求区域池（可空）
H2Session::H2Session(net::TlsStream stream,
                     Router& router,
                     MiddlewareManager& middleware,
                     RegionPool* region_pool)
    : SessionBase(router, middleware)
    , socket_(std::move(stream))
    , loop_(coro::EventLoop::current())
    , region_pool_(region_pool)
{
    // Advertise ENABLE_CONNECT_PROTOCOL (RFC 8441 WebSocket)
    local_settings_.enable_connect_protocol = 1;
    local_settings_.max_concurrent_streams = stream_mgr_.MaxConcurrent();
}

// 析构函数：默认实现，协程生命周期由 shared_ptr 管理
H2Session::~H2Session() = default;

// 主协程：HTTP/2 会话生命周期入口。发送 preface → 读数据 → 解析帧 → 分派处理 → 刷输出，
// 循环直到收到/发送 GOAWAY 或连接关闭
// 参数：无（基于成员状态）
// ═══════════════════════════════════════════════════════════════
// Start — main coroutine
//
// Event loop:
//   1. Send connection preface (SETTINGS frame)
//   2. Read TLS data from socket
//   3. Parse complete frames from the buffer
//   4. Dispatch each frame (HEADERS → HPACK decode → enqueue, etc.)
//   5. Process pending streams sequentially
//   6. Flush any pending output frames to socket
//   7. Repeat until GOAWAY / connection close
// ═══════════════════════════════════════════════════════════════

coro::Task<void> H2Session::Start()
{
    auto self = this->shared_from_this();

    if (metrics_) metrics_->OnConnectionOpen(worker_id_);

    // ── Connection preface: send our SETTINGS ──
    uint8_t settings_payload[64];
    size_t slen = EncodeSettings(settings_payload, local_settings_);
    frame_enc_.AppendSettings(settings_payload, slen);

    if (!co_await FlushOutput()) co_return;

    // ── Main read/dispatch loop ──
    try
    {
        while (!goaway_received_ && !goaway_sent_)
        {
            if (!co_await frame_reader_.Read(socket_)) {
                WakeAllStreams(H2SendWakeReason::Terminated);
                break;
            }

            for (;;) {
                H2FrameReader::Frame frame;
                auto result = frame_reader_.Next(kDefaultMaxFrameSize, frame);
                if (result == H2FrameReader::NextResult::NeedMore) break;
                if (result == H2FrameReader::NextResult::FrameTooLarge) {
                    WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::FRAME_SIZE_ERROR);
                    goaway_sent_ = true;
                    break;
                }
                ProcessFrame(frame.header, frame.payload);
                frame_reader_.Consume(frame);
            }

            if (goaway_sent_) break;

            // ── Process pending streams ──
            co_await ProcessPending();

            // WINDOW_UPDATE 在当前 Session 协程运行时投递的写协程，必须先回到
            // EventLoop 的任务队列执行；否则本协程会直接再次挂到 socket 读取，
            // 使已获额度的流错误地等到下一次入站帧才续发。
            if (send_wakeup_posted_) {
                send_wakeup_posted_ = false;
                co_await coro::sleep_for(1);
            }

            // ── Flush output ──
            // Flush once (sends the initial response headers)
            if (!co_await FlushOutput()) break;
            // Flush again if WS handler (spawned during FlushOutput) wrote RST_STREAM
            while (!output_.empty())
                if (!co_await FlushOutput()) break;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "[h2] " << e.what() << std::endl;
    }

    // ── Graceful GOAWAY ──
    if (!goaway_sent_) {
        goaway_sent_ = true;
        WakeAllStreams(H2SendWakeReason::Terminated);
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::NO_ERROR);
        co_await FlushOutput();
    }

    // Clean up remaining streams
    stream_mgr_.GcClosed();

    if (metrics_) metrics_->OnConnectionClose(worker_id_);
    co_return;
}

// ═══════════════════════════════════════════════════════════════
// Frame dispatch
// ═══════════════════════════════════════════════════════════════

// 按帧类型分派到对应的处理函数（SETTINGS/HEADERS/DATA/RST_STREAM 等）
// 参数：hdr - 帧头；payload - 帧负载指针
void H2Session::ProcessFrame(const H2FrameHeader& hdr, const uint8_t* payload)
{
    switch (hdr.type) {
    case H2FrameType::SETTINGS:      OnSettings(hdr, payload); break;
    case H2FrameType::HEADERS:       OnHeaders(hdr, payload); break;
    case H2FrameType::DATA:          OnData(hdr, payload); break;
    case H2FrameType::RST_STREAM:    OnRstStream(hdr, payload); break;
    case H2FrameType::PING:          OnPing(hdr, payload); break;
    case H2FrameType::GOAWAY:        OnGoAway(hdr, payload); break;
    case H2FrameType::WINDOW_UPDATE: OnWindowUpdate(hdr, payload); break;
    case H2FrameType::PRIORITY:      OnPriority(hdr, payload); break;
    case H2FrameType::CONTINUATION:  OnContinuation(hdr, payload); break;
    case H2FrameType::PUSH_PROMISE:
        // Server cannot receive PUSH_PROMISE — ignore (malformed peer)
        break;
    }
}

// ═══════════════════════════════════════════════════════════════
// SETTINGS (type 4)
// ═══════════════════════════════════════════════════════════════

// 处理对端 SETTINGS：解码并应用对端参数（并发数/初始窗口/帧长上限），回 SETTINGS ACK
// 参数：hdr - 帧头；payload - SETTINGS 负载
void H2Session::OnSettings(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (hdr.flags & H2Flags::ACK) {
        // Peer acknowledged our SETTINGS — nothing to do
        return;
    }

    // Decode and apply peer settings
    auto s = DecodeSettings(payload, hdr.length);

    if (s.max_concurrent_streams)
        peer_max_concurrent_ = *s.max_concurrent_streams;

    if (s.header_table_size)
        hpack_decoder_.SetMaxTableSize(*s.header_table_size);

    if (s.initial_window_size) {
        if (*s.initial_window_size > 0x7fffffffU) {
            WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::FLOW_CONTROL_ERROR);
            goaway_sent_ = true;
            return;
        }
        // 对端 SETTINGS 的 INITIAL_WINDOW_SIZE 作用于【我方发送账】，与接收账无关。
        // SetPeerInitialWindow 只调整流级发送窗口，不碰接收账，因此不会再触到
        // ShouldUpdate 的补充阈值 —— 旧实现正是因为这个才绕开它（见附录 A 的 B18）。
        peer_initial_window_ = *s.initial_window_size;
        flow_control_.SetPeerInitialWindow(*s.initial_window_size);
    }

    if (s.max_frame_size) {
        if (*s.max_frame_size < 16384 || *s.max_frame_size > 16777215) {
            WriteGoAway(stream_mgr_.LastClientStreamId(),
                        H2Error::PROTOCOL_ERROR);
            goaway_sent_ = true;
            return;
        }
        peer_max_frame_size_ = *s.max_frame_size;
        frame_enc_.SetPeerMaxFrameSize(peer_max_frame_size_);
    }

    // Respond with SETTINGS ACK
    WriteSettingsAck();
}

// ═══════════════════════════════════════════════════════════════
// HEADERS (type 1)
// ═══════════════════════════════════════════════════════════════

// 处理 HEADERS 帧：登记流、计算并解码 HPACK 块，END_STREAM 时入队待处理（含 RFC 8441 WS 分支）
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnHeaders(const H2FrameHeader& hdr, const uint8_t* payload)
{
    int32_t sid = hdr.stream_id;

    // ID violations are connection errors; only the concurrency limit is stream-local.
    auto open_result = stream_mgr_.OnStreamOpen(sid);
    if (open_result != H2StreamManager::OpenResult::Accepted) {
        if (open_result == H2StreamManager::OpenResult::ProtocolError) {
            WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::PROTOCOL_ERROR);
            goaway_sent_ = true;
        } else {
            WriteRstStream(sid, H2Error::REFUSED_STREAM);
        }
        return;
    }

    // Get or create stream context
    auto [it, created] = streams_.try_emplace(sid);
    if (created)
        it->second.InitRegion(region_pool_);
    auto& ctx = it->second;

    // Compute HPACK block location (skip padding/priority fields)
    size_t hpack_off = HeadersBlockStart(hdr);
    size_t hpack_len = HeadersBlockLength(hdr, payload);

    if (hdr.flags & H2Flags::PRIORITY) {
        // Parse priority (optional — we don't use it)
        // payload[0..4] contains exclusive+dep+weight
        (void)DecodePriority(payload + HeadersBlockStart(hdr) - 5);
    }

    if (hdr.flags & H2Flags::END_HEADERS) {
        // Complete HPACK block in this frame
        if (!hpack_decoder_.Decode(payload + hpack_off, hpack_len, ctx)) {
            WriteRstStream(sid, H2Error::COMPRESSION_ERROR);
            return;
        }
    } else {
        // CONTINUATION follows — buffer the HPACK block
        continuation_stream_id_ = sid;
        continuation_block_.assign(
            payload + hpack_off, payload + hpack_off + hpack_len);
        return;  // Wait for CONTINUATION frames
    }

    // Check for END_STREAM (or Extended CONNECT which needs no END_STREAM per RFC 8441)
    if ((hdr.flags & H2Flags::END_STREAM) || ctx.ws_extended_) {
        if (hdr.flags & H2Flags::END_STREAM)
            stream_mgr_.OnStreamEndStream(sid);
        stream_mgr_.Enqueue(sid);
    } else {
        // Request has body — keep stream open for DATA frames
        // (will be enqueued when DATA with END_STREAM arrives)
    }
}

// ═══════════════════════════════════════════════════════════════
// CONTINUATION (type 9)
// ═══════════════════════════════════════════════════════════════

// 处理 CONTINUATION 帧：累积 HPACK 块，END_HEADERS 时统一解码
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnContinuation(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (static_cast<int32_t>(hdr.stream_id) != continuation_stream_id_) {
        // CONTINUATION must belong to the same stream
        WriteGoAway(stream_mgr_.LastClientStreamId(),
                    H2Error::PROTOCOL_ERROR);
        goaway_sent_ = true;
        return;
    }

    continuation_block_.insert(continuation_block_.end(),
                               payload, payload + hdr.length);

    if (hdr.flags & H2Flags::END_HEADERS) {
        // HPACK block complete — decode it
        int32_t sid = continuation_stream_id_;
        continuation_stream_id_ = 0;

        auto it = streams_.find(sid);
        if (it == streams_.end()) {
            WriteRstStream(sid, H2Error::PROTOCOL_ERROR);
            return;
        }

        if (!hpack_decoder_.Decode(
                continuation_block_.data(),
                continuation_block_.size(), it->second)) {
            WriteRstStream(sid, H2Error::COMPRESSION_ERROR);
            return;
        }

        continuation_block_.clear();

        // END_STREAM is only in the HEADERS flags, not CONTINUATION.
        // The HEADERS END_STREAM flag was already checked in OnHeaders;
        // if it wasn't set, we wait for DATA with END_STREAM.
    }
    // else: more CONTINUATION frames follow — keep buffering
}

// ═══════════════════════════════════════════════════════════════
// DATA (type 0)
// ═══════════════════════════════════════════════════════════════

// 处理 DATA 帧：累积请求体（或投递到 WS 队列）、做流控、必要时补 WINDOW_UPDATE，END_STREAM 时入队/通知 WS
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnData(const H2FrameHeader& hdr, const uint8_t* payload)
{
    int32_t sid = hdr.stream_id;
    if (sid == 0) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::PROTOCOL_ERROR);
        goaway_sent_ = true;
        return;
    }

    // Find stream context
    auto it = streams_.find(sid);
    if (it == streams_.end()) {
        WriteRstStream(sid, H2Error::STREAM_CLOSED);
        return;
    }

    auto& ctx = it->second;

    size_t data_off = DataOffset(hdr);
    size_t data_len = DataLength(hdr, payload);

    // Reject DATA that exceeds either stream or connection receive credit.
    // RFC 7540 §6.9.1 counts the complete DATA payload, including Pad Length
    // and Padding. DataLength deliberately excludes those bytes for body parsing.
    auto actual_len = hdr.length;
    if (actual_len > flow_control_.RecvWindow(sid)) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::FLOW_CONTROL_ERROR);
        goaway_sent_ = true;
        return;
    }
    // 只调一次：连接账在 ConsumeRecv 内部一并扣。
    // （此处原本还有一行 ConsumeRecv(0, ...)，导致连接级 WINDOW_UPDATE 的
    //  增量翻倍，对端连接窗口无界膨胀 —— 见设计文档附录 A 的 B1。）
    flow_control_.ConsumeRecv(sid, actual_len);

    // Check if stream is already being handled (WS)
    if (ctx.ws_active_) {
        // WS mode — push data to the WS handler's read queue
        ctx.ws_data_queue_.emplace_back(
            reinterpret_cast<const char*>(payload + data_off), data_len);
        WakeWsStream(ctx);
    } else {
        // Normal mode — accumulate body
        ctx.AppendBody(payload + data_off, data_len);

        // Body size check：既按头声明的 Content-Length 限制，也按已累积的实际
        // 字节数限制。AppendBody 现在跨帧累加（body_buf_），若只查声明长度，
        // 恶意对端可声明小 Content-Length 却持续发 DATA 帧，造成无界累积。
        if (max_body_size_ > 0 &&
            (ctx.ContentLength() > max_body_size_ || ctx.Body().size() > max_body_size_)) {
            WriteRstStream(sid, H2Error::REFUSED_STREAM);
            return;
        }
    }

    // Send WINDOW_UPDATE if needed
    if (flow_control_.ShouldUpdate(sid)) {
        uint32_t credit = flow_control_.PopCredit(sid);
        WriteWindowUpdate(sid, credit);
    }
    if (flow_control_.ShouldUpdate(0)) {
        uint32_t credit = flow_control_.PopCredit(0);
        WriteWindowUpdate(0, credit);
    }

    // Check END_STREAM
    if (hdr.flags & H2Flags::END_STREAM) {
        stream_mgr_.OnStreamEndStream(sid);

        if (!ctx.ws_active_) {
            // Normal stream complete — enqueue for handling
            stream_mgr_.Enqueue(sid);
        } else {
            // WS stream — signal closure to WS handler
            ctx.stream_closed_ = true;
            WakeWsStream(ctx);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// RST_STREAM (type 3)
// ═══════════════════════════════════════════════════════════════

// 处理 RST_STREAM：关闭流并唤醒 WS 协程（若该流处于 WS 模式）
// 参数：hdr - 帧头；payload - 帧负载（含错误码）
void H2Session::OnRstStream(const H2FrameHeader& hdr, const uint8_t* payload)
{
    int32_t sid = hdr.stream_id;
    auto rst = DecodeRstStream(payload);
    (void)rst;  // Log the error code if desired

    stream_mgr_.OnStreamClose(sid);

    // Wake WS handler if active
    auto it = streams_.find(sid);
    if (it != streams_.end()) {
        it->second.stream_closed_ = true;
        WakeSendStream(it->second, H2SendWakeReason::Terminated);
        if (it->second.ws_active_)
            WakeWsStream(it->second);
    }
}

// ═══════════════════════════════════════════════════════════════
// PING (type 6)
// ═══════════════════════════════════════════════════════════════

// 处理 PING：对非 ACK 帧回显 8 字节负载（PING ACK）
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnPing(const H2FrameHeader& hdr, const uint8_t* payload)
{
    // PING ACK must NOT be ACK'd again
    if (hdr.flags & H2Flags::ACK) return;

    // Echo back the 8-byte opaque data
    if (hdr.length >= 8) {
        WritePingAck({*reinterpret_cast<const H2Ping*>(payload)});
    }
}

// ═══════════════════════════════════════════════════════════════
// GOAWAY (type 7)
// ═══════════════════════════════════════════════════════════════

// 处理 GOAWAY：标记对端关闭，若本端尚未发送则回 GOAWAY
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnGoAway(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (hdr.length < 8) return;
    (void)payload;
    goaway_received_ = true;
    WakeAllStreams(H2SendWakeReason::Terminated);

    // Peer is shutting down — stop accepting new streams
    if (!goaway_sent_) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::NO_ERROR);
        goaway_sent_ = true;
    }
}

// ═══════════════════════════════════════════════════════════════
// WINDOW_UPDATE (type 8)
// ═══════════════════════════════════════════════════════════════

// 处理 WINDOW_UPDATE：将对端释放的额度记入发送侧账本。
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnWindowUpdate(const H2FrameHeader& hdr, const uint8_t* payload)
{
    if (hdr.length != 4) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::FRAME_SIZE_ERROR);
        goaway_sent_ = true;
        return;
    }

    const uint32_t increment = ((static_cast<uint32_t>(payload[0]) << 24)
                              | (static_cast<uint32_t>(payload[1]) << 16)
                              | (static_cast<uint32_t>(payload[2]) << 8)
                              | static_cast<uint32_t>(payload[3])) & 0x7fffffffU;
    if (increment == 0) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::PROTOCOL_ERROR);
        goaway_sent_ = true;
        return;
    }

    if (!flow_control_.AddSendCredit(hdr.stream_id, increment)) {
        WriteGoAway(stream_mgr_.LastClientStreamId(), H2Error::FLOW_CONTROL_ERROR);
        goaway_sent_ = true;
        WakeAllStreams(H2SendWakeReason::Terminated);
    } else if (hdr.stream_id == 0) {
        WakeAllStreams(H2SendWakeReason::Window);
    } else {
        auto it = streams_.find(hdr.stream_id);
        if (it != streams_.end())
            WakeSendStream(it->second, H2SendWakeReason::Window);
    }
}

// ═══════════════════════════════════════════════════════════════
// PRIORITY (type 2)
// ═══════════════════════════════════════════════════════════════

// 处理 PRIORITY：忽略优先级（所有流顺序处理）
// 参数：hdr - 帧头；payload - 帧负载
void H2Session::OnPriority(const H2FrameHeader& hdr, const uint8_t* payload)
{
    // We ignore priority — all streams are processed sequentially.
    // Parsing would be:
    //   auto pri = DecodePriority(payload);
    (void)hdr;
    (void)payload;
}

// ═══════════════════════════════════════════════════════════════
// FlushOutput — drain output_ buffer to socket
// ═══════════════════════════════════════════════════════════════

// 把 output_ 缓冲写入 socket；用 flushing_ 防重入，交换局部缓冲以允许 WS 协程并发追加
// 参数：无；返回：写入成功与否
coro::Task<bool> H2Session::FlushOutput()
{
    if (flushing_) co_return true;
    flushing_ = true;

    for (;;) {
        if (output_.empty())
            break;
        // Swap to local buffer: spawned WS handler can safely append
        // to output_ while we send the current batch asynchronously.
        std::vector<uint8_t> send_buf;
        send_buf.swap(output_);

        bool ok = co_await socket_.write_all(std::string_view(
            reinterpret_cast<const char*>(send_buf.data()), send_buf.size()));
        if (!ok) {
            flushing_ = false;
            WakeAllStreams(H2SendWakeReason::Terminated);
            co_return false;
        }
    }

    flushing_ = false;
    co_return true;
}

// ═══════════════════════════════════════════════════════════════════
// WakeWsStream — 推送侧唤醒挂起等待中的 WS 协程
// ═══════════════════════════════════════════════════════════════════

// 推送侧唤醒等待中的 WS 协程：取消定时器成功则 post 协程句柄；定时器已到期则不再重复唤醒
// 参数：ctx - 目标流的上下文（含唤醒引用）
void H2Session::WakeWsStream(H2StreamContext& ctx)
{
    if (!ctx.ws_wakeup_.loop || ctx.ws_wakeup_.timer_id == 0) return;
    auto id = ctx.ws_wakeup_.timer_id;
    // cancel_timer 成功（true）→ 定时器作废、等待协程仍挂起，由我们负责唤醒；
    // 返回 false → 定时器已到期/已被消费（恢复已安排/已发生），不得重复唤醒。
    if (ctx.ws_wakeup_.loop->cancel_timer(id)) {
        auto h = std::coroutine_handle<>::from_address(
            reinterpret_cast<void*>(id));
        ctx.ws_wakeup_.loop->post(h);
    }
}

struct H2Session::SendWaitAwaiter {
    H2Session& session;
    H2StreamContext& context;
    int32_t stream_id;
    H2SendWakeReason immediate_reason = H2SendWakeReason::None;

    bool await_ready() noexcept
    {
        if (!session.StreamWritable(stream_id)) {
            immediate_reason = H2SendWakeReason::Terminated;
            return true;
        }
        if (session.flow_control_.SendWindow(stream_id) != 0) {
            immediate_reason = H2SendWakeReason::Window;
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept
    {
        auto& wait = context.send_wakeup_;
        wait.loop = &session.loop_;
        wait.waiter = h;
        wait.reason = H2SendWakeReason::None;
        wait.timer_id = g_h2_waiter_id.fetch_add(1, std::memory_order_relaxed);
        wait.loop->wait_timer_cancelable(session.kSendWindowTimeoutMs,
                                         h, wait.timer_id);
    }

    H2SendWakeReason await_resume() noexcept
    {
        auto& wait = context.send_wakeup_;
        if (wait.timer_id != 0)
            wait.loop->cancel_timer(wait.timer_id);
        const auto reason = wait.reason == H2SendWakeReason::None
            ? immediate_reason : wait.reason;
        wait.timer_id = 0;
        wait.waiter = {};
        wait.reason = H2SendWakeReason::None;
        return reason == H2SendWakeReason::None
            ? H2SendWakeReason::Timeout
            : reason;
    }
};

coro::Task<bool> H2Session::WaitForSendable(int32_t sid)
{
    auto it = streams_.find(sid);
    if (it == streams_.end()) co_return false;
    if (!StreamWritable(sid)) co_return false;
    if (flow_control_.SendWindow(sid) != 0) co_return true;

    // A timeout is represented by no wake reason when the cancelable timer fires.
    const auto reason = co_await SendWaitAwaiter{*this, it->second, sid};
    if (reason == H2SendWakeReason::Timeout) {
        WriteRstStream(sid, H2Error::CANCEL);
        auto current = streams_.find(sid);
        if (current != streams_.end())
            current->second.stream_closed_ = true;
        stream_mgr_.OnStreamClose(sid);
        co_await FlushOutput();
        co_return false;
    }
    co_return StreamWritable(sid) && flow_control_.SendWindow(sid) != 0;
}

bool H2Session::StreamWritable(int32_t sid) const
{
    if (goaway_sent_ || goaway_received_) return false;
    auto it = streams_.find(sid);
    return it != streams_.end() && !it->second.stream_closed_;
}

void H2Session::WakeSendStream(H2StreamContext& ctx, H2SendWakeReason reason)
{
    auto& wait = ctx.send_wakeup_;
    if (!wait.waiter || wait.reason != H2SendWakeReason::None) return;
    wait.reason = reason;
    if (wait.loop && wait.timer_id != 0 && wait.loop->cancel_timer(wait.timer_id)) {
        wait.loop->post(wait.waiter);
        send_wakeup_posted_ = true;
    }
}

void H2Session::WakeAllStreams(H2SendWakeReason reason)
{
    for (auto& [sid, ctx] : streams_)
        WakeSendStream(ctx, reason);
}

void H2Session::FinishStream(int32_t sid)
{
    streams_.erase(sid);
    stream_mgr_.RemoveStream(sid);
    flow_control_.RemoveStream(sid);
}

// ═══════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════
// ProcessPending — drain the stream pending queue
// ═══════════════════════════════════════════════════════════════

// 分派已就绪流：每条流运行在同一事件循环的独立协程中，不能阻塞连接读循环。
// 参数：无
coro::Task<void> H2Session::ProcessPending()
{
    while (stream_mgr_.HasPending()) {
        auto sid = stream_mgr_.Dequeue();
        auto self = std::static_pointer_cast<H2Session>(shared_from_this());
        auto task = self->HandleStream(sid);
        // 立即推进到第一次挂起点。EventLoop::post 在当前协程还未挂起时只会
        // 入队，可能要等下一次网络事件才被取出；先启动保证流处理器已就绪，
        // 后续的窗口/IO 等待仍由事件循环独立恢复。
        task.handle().resume();
    }
    co_return;
}

// ═══════════════════════════════════════════════════════════════
// Output helpers
// ═══════════════════════════════════════════════════════════════

// 追加一条 HEADERS 帧到 output_（HPACK 块已由调用方编码好）
// 参数：sid - 目标流 ID；hpack - 已编码的 HPACK 块；end_headers - 是否带 END_HEADERS 标志
void H2Session::WriteHeaders(int32_t sid,
                              const std::vector<uint8_t>& hpack,
                              bool end_headers)
{
    frame_enc_.AppendHeaders(sid, hpack, end_headers);
}

coro::Task<bool> H2Session::SendData(int32_t sid, std::string_view data)
{
    if (!StreamWritable(sid)) co_return false;

    size_t offset = 0;
    while (offset < data.size()) {
        if (!StreamWritable(sid)) co_return false;

        const auto available = flow_control_.SendWindow(sid);
        if (available == 0) {
            if (!(co_await WaitForSendable(sid)))
                co_return false;
            continue;
        }

        const size_t chunk = std::min({
            data.size() - offset,
            static_cast<size_t>(available),
            static_cast<size_t>(frame_enc_.PeerMaxFrameSize())});
        if (chunk == 0) co_return false;

        frame_enc_.AppendData(sid,
            reinterpret_cast<const uint8_t*>(data.data() + offset),
            chunk, false);
        flow_control_.ConsumeSend(sid, static_cast<uint32_t>(chunk));
        offset += chunk;
    }

    co_return co_await FlushOutput();
}

coro::Task<void> H2Session::EndStream(int32_t sid)
{
    if (!StreamWritable(sid)) co_return;
    frame_enc_.AppendData(sid, nullptr, 0, true);
    co_await FlushOutput();
}

// 追加一条 RST_STREAM 帧到 output_，用于中止/关闭流
// 参数：sid - 目标流 ID；err - 错误码
void H2Session::WriteRstStream(int32_t sid, H2Error err)
{
    frame_enc_.AppendRstStream(sid, err);
}

// 追加一条 GOAWAY 帧到 output_，通告对端停止新流
// 参数：last_sid - 已处理的最后流 ID；err - 错误码
void H2Session::WriteGoAway(int32_t last_sid, H2Error err)
{
    frame_enc_.AppendGoAway(last_sid, err);
}

// 追加一条 WINDOW_UPDATE 帧到 output_，补充流/连接级接收窗口
// 参数：sid - 目标流 ID（0 为连接级）；increment - 窗口增量
void H2Session::WriteWindowUpdate(int32_t sid, uint32_t increment)
{
    frame_enc_.AppendWindowUpdate(sid, increment);
}

// 追加一条 PING ACK 帧到 output_，回应对端 PING
// 参数：ping - 要回显的 8 字节 opaque 数据
void H2Session::WritePingAck(const H2Ping& ping)
{
    frame_enc_.AppendPingAck(ping);
}

// 追加一条 SETTINGS ACK 帧到 output_，确认对端 SETTINGS
// 参数：无
void H2Session::WriteSettingsAck()
{
    frame_enc_.AppendSettingsAck();
}

// ═══════════════════════════════════════════════════════════════
// WriteResponseHeaders — HPACK-encode + emit HEADERS frame
// ═══════════════════════════════════════════════════════════════

// 对响应头做 HPACK 编码并追加 HEADERS 帧（:status + 小写响应头，过滤 hop-by-hop 头）
// 参数：sid - 目标流 ID；resp - 响应对象
void H2Session::WriteResponseHeaders(int32_t sid, const Response& resp)
{
    // Build header list for HPACK encoder.
    // IMPORTANT: Do NOT use region_.Dup() for header names — that would
    // advance region.Used() past header_end_, causing BodyWire() to
    // report wrong body size.  Keep lowercase name strings alive in
    // a local vector instead.  Reserve upfront to avoid reallocation
    // that would move SSO strings and dangle the string_views.
    std::vector<std::pair<std::string_view, std::string_view>> headers;
    std::vector<std::string> header_names;
    header_names.reserve(static_cast<size_t>(resp.HeaderCount()) + 2);

    // :status pseudo-header
    char status_buf[8];
    int status_len = std::snprintf(status_buf, sizeof(status_buf), "%d", resp.StatusCode());
    headers.emplace_back(":status", std::string_view{status_buf, static_cast<size_t>(status_len)});

    // Response headers (lowercase names per RFC 7540 §8.1.2)
    for (int i = 0; i < resp.HeaderCount(); i++) {
        auto [name, value] = resp.HeaderAt(i);
        if (name == "Connection" || name == "Transfer-Encoding" || name == "Date" ||
            name == "connection" || name == "transfer-encoding" || name == "date")
            continue;
        header_names.emplace_back(name);
        auto& lower = header_names.back();
        for (auto& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        headers.emplace_back(lower, value);
    }

    auto hpack = hpack_encoder_.Encode(headers);
    WriteHeaders(sid, hpack, true);
}

// 处理单条 HTTP/2 请求流：中间件→路由→响应，支持 SSE 推送与 RFC 8441 WS 分支；末尾清理流与区域
// 参数：stream_id - 待处理的流 ID
coro::Task<void> H2Session::HandleStream(int32_t stream_id)
{
    auto self = std::static_pointer_cast<H2Session>(shared_from_this());
    co_await H2StreamProcessor(std::move(self), stream_id).Run();
}
