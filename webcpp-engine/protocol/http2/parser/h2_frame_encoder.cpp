#include "protocol/http2/parser/h2_frame_encoder.hpp"
#include <algorithm>
#include <cstring>

// 所有方法都只向 output_ 末尾追加完整的 HTTP/2 wire 帧；真正的发送由会话层负责。
// 帧头统一占 9 字节，负载长度由 EncodeFrameHeader 写入 24 位大端字段。

// 记录对端 SETTINGS_MAX_FRAME_SIZE。0 不是有效的设置值；将其回退为
// RFC 7540 规定的默认值，避免 DATA 分片时出现零长度分片。
void H2FrameEncoder::SetPeerMaxFrameSize(uint32_t size)
{
    peer_max_frame_size_ = size == 0 ? kDefaultMaxFrameSize : size;
}

// 追加 SETTINGS 帧。payload 已经是编码好的 6 字节设置项序列（可为空），
// 本方法不解释其内容，也不自动拆分负载。
void H2FrameEncoder::AppendSettings(const uint8_t* payload, size_t len)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + len);
    EncodeFrameHeader(output_.data() + pos,
                      {static_cast<uint32_t>(len), H2FrameType::SETTINGS, 0, 0});
    if (len != 0) std::memcpy(output_.data() + pos + kFrameHeaderSize, payload, len);
}

// 追加空负载的 SETTINGS ACK；RFC 7540 要求带 ACK 的 SETTINGS 帧没有负载。
void H2FrameEncoder::AppendSettingsAck()
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize);
    EncodeFrameHeader(output_.data() + pos, {0, H2FrameType::SETTINGS, H2Flags::ACK, 0});
}

// 追加一条 HEADERS 帧。block 是调用方已经完成的 HPACK 头部块，
// end_headers=false 时由调用方负责后续 CONTINUATION 帧的组织。
void H2FrameEncoder::AppendHeaders(int32_t stream_id, const std::vector<uint8_t>& block, bool end_headers)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + block.size());
    EncodeFrameHeader(output_.data() + pos,
                      {static_cast<uint32_t>(block.size()), H2FrameType::HEADERS,
                       static_cast<uint8_t>(end_headers ? H2Flags::END_HEADERS : 0),
                       static_cast<uint32_t>(stream_id)});
    if (!block.empty()) std::memcpy(output_.data() + pos + kFrameHeaderSize, block.data(), block.size());
}

// 按对端允许的最大负载长度拆分 DATA。END_STREAM 必须落在最后一片，
// 因而中间分片不会错误地结束流；len=0 仍会产生一条零长度 DATA 帧。
void H2FrameEncoder::AppendData(int32_t stream_id, const uint8_t* data, size_t len, bool end_stream)
{
    size_t off = 0;
    for (;;) {
        const size_t chunk = std::min(len - off, static_cast<size_t>(peer_max_frame_size_));
        const bool last = off + chunk >= len;
        const size_t pos = output_.size();
        output_.resize(pos + kFrameHeaderSize + chunk);
        EncodeFrameHeader(output_.data() + pos,
                          {static_cast<uint32_t>(chunk), H2FrameType::DATA,
                           static_cast<uint8_t>((last && end_stream) ? H2Flags::END_STREAM : 0),
                           static_cast<uint32_t>(stream_id)});
        if (chunk != 0 && data) std::memcpy(output_.data() + pos + kFrameHeaderSize, data + off, chunk);
        if (last) return;
        off += chunk;
    }
}

// RST_STREAM 的负载固定为 4 字节错误码，且只能作用于指定流。
void H2FrameEncoder::AppendRstStream(int32_t stream_id, H2Error error)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 4);
    EncodeFrameHeader(output_.data() + pos, {4, H2FrameType::RST_STREAM, 0, static_cast<uint32_t>(stream_id)});
    EncodeRstStream(output_.data() + pos + kFrameHeaderSize, error);
}

// GOAWAY 使用连接级流 ID（帧头 stream_id 必须为 0），其 8 字节负载包含
// 最后处理的流 ID 和连接错误码。
void H2FrameEncoder::AppendGoAway(int32_t last_stream_id, H2Error error)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 8);
    EncodeFrameHeader(output_.data() + pos, {8, H2FrameType::GOAWAY, 0, 0});
    EncodeGoAway(output_.data() + pos + kFrameHeaderSize, {static_cast<uint32_t>(last_stream_id), error});
}

// 追加 4 字节窗口增量。stream_id=0 表示连接级窗口，否则表示指定流的窗口。
void H2FrameEncoder::AppendWindowUpdate(int32_t stream_id, uint32_t increment)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 4);
    EncodeFrameHeader(output_.data() + pos, {4, H2FrameType::WINDOW_UPDATE, 0, static_cast<uint32_t>(stream_id)});
    EncodeWindowUpdate(output_.data() + pos + kFrameHeaderSize, increment);
}

// 追加 PING ACK，并原样回显对端提供的 8 字节 opaque 数据；PING 始终是连接级帧。
void H2FrameEncoder::AppendPingAck(const H2Ping& ping)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 8);
    EncodeFrameHeader(output_.data() + pos, {8, H2FrameType::PING, H2Flags::ACK, 0});
    EncodePing(output_.data() + pos + kFrameHeaderSize, ping);
}
