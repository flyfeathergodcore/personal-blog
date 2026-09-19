#include "http/protocol/http2/parser/h2_frame_reader.hpp"
#include <cstring>
#include <openssl/ssl.h>

coro::Task<bool> H2FrameReader::Read(net::TlsStream& socket)
{
    bool read_ok = false;
    for (int pass = 0; pass < 2; ++pass) {
        if (used_ == buffer_.size()) co_return false;
        auto result = co_await socket.read_some(buffer_.data() + used_, buffer_.size() - used_);
        if (!result.ok()) break;
        used_ += result.bytes;
        read_ok = true;
        if (::SSL_pending(socket.native_handle()) <= 0) break;
    }
    co_return read_ok;
}

bool H2FrameReader::Append(const uint8_t* data, size_t len)
{
    if (len > buffer_.size() - used_) return false;
    std::memcpy(buffer_.data() + used_, data, len);
    used_ += len;
    return true;
}

bool H2FrameReader::ConsumePreface()
{
    if (preface_checked_) return true;
    const size_t checked = std::min(used_, kH2ClientPreface.size());
    if (checked != 0 && std::memcmp(buffer_.data(), kH2ClientPreface.data(), checked) == 0) {
        if (used_ < kH2ClientPreface.size()) return false;
        std::memmove(buffer_.data(), buffer_.data() + kH2ClientPreface.size(),
                     used_ - kH2ClientPreface.size());
        used_ -= kH2ClientPreface.size();
    }
    preface_checked_ = true;
    return true;
}

H2FrameReader::NextResult H2FrameReader::Next(uint32_t max_frame_size, Frame& frame)
{
    if (!ConsumePreface()) return NextResult::NeedMore;
    if (used_ < kFrameHeaderSize) return NextResult::NeedMore;

    auto header = DecodeFrameHeader(buffer_.data());
    if (header.length > max_frame_size) return NextResult::FrameTooLarge;
    const size_t wire_size = kFrameHeaderSize + header.length;
    if (used_ < wire_size) return NextResult::NeedMore;

    frame = {header, buffer_.data() + kFrameHeaderSize, wire_size};
    return NextResult::Frame;
}

void H2FrameReader::Consume(const Frame& frame)
{
    if (frame.wire_size > used_) return;
    const size_t remaining = used_ - frame.wire_size;
    if (remaining != 0) std::memmove(buffer_.data(), buffer_.data() + frame.wire_size, remaining);
    used_ = remaining;
}
