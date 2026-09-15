#include "protocol/http2/parser/h2_frame_encoder.hpp"
#include <algorithm>
#include <cstring>

void H2FrameEncoder::SetPeerMaxFrameSize(uint32_t size)
{
    peer_max_frame_size_ = size == 0 ? kDefaultMaxFrameSize : size;
}

void H2FrameEncoder::AppendSettings(const uint8_t* payload, size_t len)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + len);
    EncodeFrameHeader(output_.data() + pos,
                      {static_cast<uint32_t>(len), H2FrameType::SETTINGS, 0, 0});
    if (len != 0) std::memcpy(output_.data() + pos + kFrameHeaderSize, payload, len);
}

void H2FrameEncoder::AppendSettingsAck()
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize);
    EncodeFrameHeader(output_.data() + pos, {0, H2FrameType::SETTINGS, H2Flags::ACK, 0});
}

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

void H2FrameEncoder::AppendRstStream(int32_t stream_id, H2Error error)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 4);
    EncodeFrameHeader(output_.data() + pos, {4, H2FrameType::RST_STREAM, 0, static_cast<uint32_t>(stream_id)});
    EncodeRstStream(output_.data() + pos + kFrameHeaderSize, error);
}

void H2FrameEncoder::AppendGoAway(int32_t last_stream_id, H2Error error)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 8);
    EncodeFrameHeader(output_.data() + pos, {8, H2FrameType::GOAWAY, 0, 0});
    EncodeGoAway(output_.data() + pos + kFrameHeaderSize, {static_cast<uint32_t>(last_stream_id), error});
}

void H2FrameEncoder::AppendWindowUpdate(int32_t stream_id, uint32_t increment)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 4);
    EncodeFrameHeader(output_.data() + pos, {4, H2FrameType::WINDOW_UPDATE, 0, static_cast<uint32_t>(stream_id)});
    EncodeWindowUpdate(output_.data() + pos + kFrameHeaderSize, increment);
}

void H2FrameEncoder::AppendPingAck(const H2Ping& ping)
{
    const size_t pos = output_.size();
    output_.resize(pos + kFrameHeaderSize + 8);
    EncodeFrameHeader(output_.data() + pos, {8, H2FrameType::PING, H2Flags::ACK, 0});
    EncodePing(output_.data() + pos + kFrameHeaderSize, ping);
}
