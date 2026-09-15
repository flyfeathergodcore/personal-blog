#pragma once

#include "coro/task.h"
#include "net/tls_stream.h"
#include "protocol/http2/parser/BFL.hpp"
#include <array>

// HTTP/2 帧读取器：维护连接读缓冲，只负责切分和校验，不解释帧语义。
class H2FrameReader {
public:
    struct Frame {
        H2FrameHeader header;
        const uint8_t* payload = nullptr;
        size_t wire_size = 0;
    };

    enum class NextResult : uint8_t { Frame, NeedMore, FrameTooLarge };

    coro::Task<bool> Read(net::TlsStream& socket);
    NextResult Next(uint32_t max_frame_size, Frame& frame);
    void Consume(const Frame& frame);

    // 仅供纯解析单测构造缓冲内容。
    bool Append(const uint8_t* data, size_t len);
    size_t Buffered() const { return used_; }

private:
    static constexpr size_t kReadBufSize = 65536;
    std::array<uint8_t, kReadBufSize> buffer_{};
    size_t used_ = 0;
    bool preface_checked_ = false;

    bool ConsumePreface();
};
