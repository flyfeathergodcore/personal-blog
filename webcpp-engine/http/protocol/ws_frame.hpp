#pragma once
#include "coro/task.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <array>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
// WebSocket frame types (RFC 6455)
// ═══════════════════════════════════════════════════════════════════

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct WsFrame {
    bool    fin     = true;
    WsOpcode opcode = WsOpcode::Binary;
    std::string payload;   // unmasked payload data
};

// ── Frame I/O (template, inline) ──

/// 从流中异步精确读取 n 字节，超时透传（timeout_ms < 0 = 无限等待）
/// 参数：stream - 数据流；n - 需读取的字节数；timeout_ms - 超时（毫秒，<0 无限）
/// 返回：读满 n 字节的字符串；出错/超时返回空串
template<typename Stream>
coro::Task<std::string> ReadExactly(Stream& stream, size_t n,
                                    int64_t timeout_ms = -1)
{
    std::string buf(n, '\0');
    if (n == 0) co_return buf;

    size_t total = 0;
    while (total < n)
    {
        auto r = co_await stream.read_some(buf.data() + total, n - total,
                                           timeout_ms);
        if (!r.ok()) co_return std::string();  // 连接错误/超时
        total += r.bytes;
    }
    co_return buf;
}

/// 就地解除 payload 掩码（与 4 字节掩码键异或）
/// 参数：payload - 待解码的负载数据；mask_key - 4 字节掩码键
inline void UnmaskPayload(std::string& payload, const uint8_t mask_key[4])
{
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] ^= mask_key[i & 3];
}

/// 从流中读取一个 WebSocket 帧。
/// 出错/超时/半读（header 读失败、扩展长度部分读、mask key 部分读、payload 短读）
/// 一律返回 std::nullopt —— 绝不返回部分解析的帧，从而与合法零长度 Binary 帧
/// 彻底区分。超时透传到每个 ReadExactly。
/// 参数：stream - 数据流；timeout_ms - 超时（毫秒，<0 无限）
/// 返回：解析出的帧；出错/超时/半读返回 std::nullopt
template<typename Stream>
coro::Task<std::optional<WsFrame>> ReadFrame(Stream& stream,
                                             int64_t timeout_ms = -1)
{
    WsFrame frame;

    // 1. Read 2-byte header
    auto hdr = co_await ReadExactly(stream, 2, timeout_ms);
    if (hdr.size() < 2) co_return std::nullopt;
    auto h = reinterpret_cast<const uint8_t*>(hdr.data());

    frame.fin     = (h[0] & 0x80) != 0;
    // RSV bits ignored
    frame.opcode  = static_cast<WsOpcode>(h[0] & 0x0F);
    bool masked   = (h[1] & 0x80) != 0;
    uint64_t len  = h[1] & 0x7F;

    // 2. Extended payload length
    if (len == 126) {
        auto ext = co_await ReadExactly(stream, 2, timeout_ms);
        if (ext.size() < 2) co_return std::nullopt;
        len = (static_cast<uint64_t>(static_cast<uint8_t>(ext[0])) << 8)
            | static_cast<uint64_t>(static_cast<uint8_t>(ext[1]));
    } else if (len == 127) {
        auto ext = co_await ReadExactly(stream, 8, timeout_ms);
        if (ext.size() < 8) co_return std::nullopt;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | static_cast<uint64_t>(static_cast<uint8_t>(ext[i]));
    }

    // 3. Masking key (client→server only)
    uint8_t mask_key[4] = {};
    if (masked) {
        auto mk = co_await ReadExactly(stream, 4, timeout_ms);
        if (mk.size() < 4) co_return std::nullopt;
        std::memcpy(mask_key, mk.data(), 4);
    }

    // 4. Payload — limit to 1 MB for safety
    constexpr uint64_t kMaxPayload = 1024 * 1024;
    if (len > kMaxPayload) co_return std::nullopt;
    if (len > 0) {
        frame.payload = co_await ReadExactly(stream, static_cast<size_t>(len),
                                             timeout_ms);
        if (frame.payload.size() < static_cast<size_t>(len))
            co_return std::nullopt;  // read error
        if (masked)
            UnmaskPayload(frame.payload, mask_key);
    }

    co_return frame;
}

/// 向流中写入一个 WebSocket 帧。
/// Server → Client: mask=false；Client → Server: mask=true（中继）。
/// 参数：stream - 数据流；opcode - 帧操作码；payload - 负载数据；fin - 是否结束帧（默认 true）；mask - 是否掩码（默认 false）
template<typename Stream>
coro::Task<void> WriteFrame(Stream& stream, WsOpcode opcode,
                            std::string payload, bool fin = true,
                            bool mask = false)
{
    std::string header;
    header.reserve(14 + payload.size());

    // Byte 0: FIN + opcode
    uint8_t b0 = (fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode);
    header += static_cast<char>(b0);

    // Byte 1: MASK + payload_len
    uint64_t len = payload.size();
    uint8_t b1 = (mask ? 0x80 : 0x00);
    if (len < 126) {
        b1 |= static_cast<uint8_t>(len);
        header += static_cast<char>(b1);
    } else if (len <= 0xFFFF) {
        b1 |= 126;
        header += static_cast<char>(b1);
        header += static_cast<char>((len >> 8) & 0xFF);
        header += static_cast<char>(len & 0xFF);
    } else {
        b1 |= 127;
        header += static_cast<char>(b1);
        for (int i = 7; i >= 0; i--)
            header += static_cast<char>((len >> (i * 8)) & 0xFF);
    }

    // Masking key + masked payload
    if (mask) {
        // Use a fixed key for now (non-cryptographic, just protocol compliant)
        uint8_t mk[4] = {0x00, 0x00, 0x00, 0x00};
        header.append(reinterpret_cast<const char*>(mk), 4);
        for (size_t i = 0; i < payload.size(); i++)
            payload[i] ^= mk[i & 3];
    }

    header += payload;
    (void)co_await stream.write_all(header);
    co_return;
}

/// 便捷函数：发送一个 Close 帧
/// 参数：stream - 数据流；code - 关闭状态码（默认 1000）；reason - 关闭原因（默认空）
template<typename Stream>
coro::Task<void> WriteCloseFrame(Stream& stream,
                                 uint16_t code = 1000,
                                 std::string_view reason = {})
{
    std::string payload;
    payload += static_cast<char>((code >> 8) & 0xFF);
    payload += static_cast<char>(code & 0xFF);
    payload += reason;
    co_await WriteFrame(stream, WsOpcode::Close, std::move(payload));
}

/// 计算 WebSocket 握手应答值 Sec-WebSocket-Accept
/// 参数：client_key - 客户端 Sec-WebSocket-Key；返回：Base64(SHA1(key + GUID))
std::string ComputeWsAccept(std::string_view client_key);
