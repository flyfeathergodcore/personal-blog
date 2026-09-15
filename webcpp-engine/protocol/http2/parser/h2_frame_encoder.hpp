#pragma once

#include "protocol/http2/parser/BFL.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// HTTP/2 帧编码器：只向连接输出缓冲追加帧字节，不执行任何 IO。
class H2FrameEncoder {
public:
    explicit H2FrameEncoder(std::vector<uint8_t>& output) : output_(output) {}

    void SetPeerMaxFrameSize(uint32_t size);
    void AppendSettings(const uint8_t* payload, size_t len);
    void AppendSettingsAck();
    void AppendHeaders(int32_t stream_id, const std::vector<uint8_t>& block, bool end_headers);
    void AppendData(int32_t stream_id, const uint8_t* data, size_t len, bool end_stream);
    void AppendRstStream(int32_t stream_id, H2Error error);
    void AppendGoAway(int32_t last_stream_id, H2Error error);
    void AppendWindowUpdate(int32_t stream_id, uint32_t increment);
    void AppendPingAck(const H2Ping& ping);

private:
    std::vector<uint8_t>& output_;
    uint32_t peer_max_frame_size_ = kDefaultMaxFrameSize;
};
