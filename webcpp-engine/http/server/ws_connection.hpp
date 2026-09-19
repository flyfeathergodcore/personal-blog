#pragma once
#include "http/protocol/ws_frame.hpp"
#include "coro/task.h"
#include <string>
#include <memory>

// ═══════════════════════════════════════════════════════════════════
// WsConnectionBase — type-erased WebSocket connection interface
//
// Enables RequestHandler::HandleWebSocket(WsConnectionBase&) without
// templating the handler on stream type.
// ═══════════════════════════════════════════════════════════════════

class WsConnectionBase {
public:
    // 虚析构函数：默认实现
    virtual ~WsConnectionBase() = default;

    /// Read next frame (auto-responds ping/pong, auto-replies close).
    /// 连接关闭/超时/读错误时返回 Close-opcode 帧（空 payload），
    /// 与合法的零长度 Text/Binary 帧（仍是正常数据帧）明确区分。
    virtual coro::Task<WsFrame> Read() = 0;

    /// Send a frame.
    virtual coro::Task<void> Send(WsOpcode opcode, std::string payload,
                                  bool fin = true) = 0;

    /// Initiate close handshake.
    virtual coro::Task<void> Close(uint16_t code = 1000,
                                   std::string_view reason = {}) = 0;

    /// 向底层流写原始字节（反向代理用它把上游的 101 握手响应原样透传给客户端）。
    /// 默认实现为空操作，仅需要原始写的能力者（如 ReverseProxy）覆写。
    virtual coro::Task<bool> WriteRaw(std::string_view /*data*/) {
        co_return true;
    }

    // 连接是否仍打开
    // 参数：无；返回：是否打开
    bool IsOpen() const { return !closed_; }

protected:
    bool closed_ = false;
};

// ═══════════════════════════════════════════════════════════════════
// WsConnection — WebSocket connection state (templated implementation)
//
// Wraps a stream (TCP or TLS) and provides frame-level read/write
// with automatic ping/pong/close handshake handling.
// Template parameter: Stream（tcp::Stream 或 net::TlsStream）
// ═══════════════════════════════════════════════════════════════════

template<typename Stream>
class WsConnection : public WsConnectionBase {
public:
    /// @param stream  The underlying TCP/TLS stream.
    /// @param idle_timeout_sec  Idle timeout in seconds (0 = no timeout).
    ///        When set, Read() cancels if no frame arrives within the timeout,
    ///        returning an empty frame and closing the connection.
    explicit WsConnection(Stream& stream, unsigned int idle_timeout_sec = 0)
        : stream_(stream)
        , idle_timeout_ms_(idle_timeout_sec == 0
                               ? 0
                               : static_cast<int64_t>(idle_timeout_sec) * 1000)
    {}

    // 禁止拷贝（持有底层流引用）
    WsConnection(const WsConnection&) = delete;
    WsConnection& operator=(const WsConnection&) = delete;

    // 读取下一帧：自动应答 Ping/Pong、自动回 Close；连接关闭/超时/读错误时返回 Close-opcode 帧（空 payload）
    // 参数：无；返回：WsFrame 帧对象
    coro::Task<WsFrame> Read() override
    {
        for (;;)
        {
            // ── ReadFrame 带空闲超时（把超时下传给每个 read_some）──
            // ReadFrame 出错/超时/半读返回 nullopt，绝不给调用方部分解析的帧。
            auto opt = co_await ReadFrame(
                stream_, idle_timeout_ms_ == 0 ? -1 : idle_timeout_ms_);

            // 连接关闭/超时/读错误：置 closed_；idle_timeout>0 时回 Close(1002)（简报语义）。
            // 以 Close-opcode 帧返回（空 payload）——handler 视作"连接已结束"，
            // 与合法的零长度 Text/Binary 数据帧明确区分。
            if (!opt.has_value())
            {
                closed_ = true;
                if (idle_timeout_ms_ > 0)
                    co_await WriteCloseFrame(stream_, 1002, "timeout");
                co_return WsFrame{true, WsOpcode::Close, {}};
            }

            WsFrame frame = std::move(*opt);

            // ── 按 opcode 分发；无 payload 哨兵——零长度帧是合法数据帧 ──
            switch (frame.opcode)
            {
            case WsOpcode::Ping:
                co_await WriteFrame(stream_, WsOpcode::Pong,
                                    std::move(frame.payload), true, false);
                continue;

            case WsOpcode::Close:
            {
                uint16_t code = 1000;
                if (frame.payload.size() >= 2) {
                    code = (static_cast<uint8_t>(frame.payload[0]) << 8) |
                            static_cast<uint8_t>(frame.payload[1]);
                }
                std::string_view reason;
                if (frame.payload.size() > 2)
                    reason = {frame.payload.data() + 2,
                              frame.payload.size() - 2};
                co_await WriteCloseFrame(stream_, code, reason);
                closed_ = true;
                co_return WsFrame{true, WsOpcode::Close, {}};
            }

            case WsOpcode::Pong:
                continue;

            default:
                // Text / Binary / Continuation —— 含零长度帧，原样返回给 handler
                co_return frame;
            }
        }
    }

    // 发送一帧数据
    // 参数：opcode - 帧类型；payload - 帧负载；fin - 是否为最终帧
    coro::Task<void> Send(WsOpcode opcode, std::string payload,
                          bool fin = true) override
    {
        co_await WriteFrame(stream_, opcode, std::move(payload), fin, false);
        co_return;
    }

    // 发起关闭握手（发送 Close 帧）；已关闭则无操作
    // 参数：code - 关闭状态码；reason - 关闭原因
    coro::Task<void> Close(uint16_t code = 1000,
                           std::string_view reason = {}) override
    {
        if (closed_) co_return;
        closed_ = true;
        co_await WriteCloseFrame(stream_, code, reason);
        co_return;
    }

    // 向底层流写原始字节（反向代理透传上游 101 响应时使用）
    // 参数：data - 原始字节；返回：写入成功与否
    coro::Task<bool> WriteRaw(std::string_view data) override {
        co_return co_await stream_.write_all(data);
    }

private:
    Stream& stream_;
    int64_t idle_timeout_ms_ = 0;
};
