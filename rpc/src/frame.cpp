// 帧编解码实现：见 include/rpc/frame.h
#include "rpc/frame.h"

namespace rpc {

// 4 字节大端长度前缀：把长度写入 out 前 4 字节
static void WriteLenPrefix(uint32_t len, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>((len >> 24) & 0xff);
    out[1] = static_cast<uint8_t>((len >> 16) & 0xff);
    out[2] = static_cast<uint8_t>((len >> 8) & 0xff);
    out[3] = static_cast<uint8_t>(len & 0xff);
}

bool FrameCodec::Encode(const RpcFrame& f, std::string* out) {
    // 序列化负载；失败（无 setter 或内部状态异常）返回 false
    std::string body;
    if (!f.SerializeToString(&body)) return false;
    if (body.size() > kMaxFrameLen) return false;
    out->clear();
    out->reserve(4 + body.size());
    uint8_t hdr[4];
    WriteLenPrefix(static_cast<uint32_t>(body.size()), hdr);
    out->append(reinterpret_cast<const char*>(hdr), 4);
    out->append(body);
    return true;
}

bool FrameCodec::TryDecodeHeader(const uint8_t hdr[4], uint32_t* len) {
    // 大端解析
    uint32_t n = (static_cast<uint32_t>(hdr[0]) << 24) |
                 (static_cast<uint32_t>(hdr[1]) << 16) |
                 (static_cast<uint32_t>(hdr[2]) << 8) |
                 (static_cast<uint32_t>(hdr[3]));
    // 超上限直接拒绝（防恶意长度）
    if (n > kMaxFrameLen) return false;
    *len = n;
    return true;
}

namespace {
// 可恢复读满 n 字节：每次 read_some 有进展就累计，超时返回 Timeout 且保留 *got
// 进度（调用方下次续读）。不能直接用 read_exact：其超时返回只携带最后一次
// read_some 的字节数（0），已读字节会被丢失，无法自行恢复。
coro::Task<net::IoResult> ReadSomeResumable(net::TcpStream& s, char* dst, std::size_t n,
                                            int64_t timeout_ms, std::size_t* got) {
    while (*got < n) {
        net::IoResult r = co_await s.read_some(dst + *got, n - *got, timeout_ms);
        if (!r.ok()) co_return r;
        *got += r.bytes;
    }
    co_return net::IoResult{*got, net::IoError::None};
}
}  // namespace

// 分两段读（头 + 负载），每段超时保留已读进度；整体帧解析完成后复位
coro::Task<net::IoResult> FrameReader::Read(net::TcpStream& s, RpcFrame* out,
                                            int64_t timeout_ms) {
    if (stage_ == Stage::kHeader) {
        net::IoResult r = co_await ReadSomeResumable(s, reinterpret_cast<char*>(hdr_),
                                                     4, timeout_ms, &hdr_got_);
        if (!r.ok()) co_return r;
        if (!FrameCodec::TryDecodeHeader(hdr_, &want_)) {
            co_return net::IoResult{0, net::IoError::Other};  // 超长/损坏头
        }
        body_.assign(want_, '\0');
        body_got_ = 0;
        stage_ = Stage::kBody;
    }
    if (stage_ == Stage::kBody && want_ > body_got_) {
        net::IoResult r = co_await ReadSomeResumable(s, body_.data(), want_, timeout_ms,
                                                     &body_got_);
        if (!r.ok()) co_return r;
    }
    if (!out->ParseFromString(body_)) {
        co_return net::IoResult{0, net::IoError::Other};
    }
    const std::uint32_t len = want_;
    stage_ = Stage::kHeader;
    hdr_got_ = 0;
    want_ = 0;
    body_got_ = 0;
    body_.clear();
    co_return net::IoResult{static_cast<size_t>(len), net::IoError::None};
}

coro::Task<net::IoResult> ReadFrame(net::TcpStream& s, RpcFrame* out,
                                    int64_t timeout_ms) {
    // 先读 4 字节长度头
    uint8_t hdr[4];
    auto r = co_await s.read_exact(hdr, 4, timeout_ms);
    if (!r.ok()) co_return r;

    // 校验并解析长度
    uint32_t len = 0;
    if (!FrameCodec::TryDecodeHeader(hdr, &len)) {
        co_return net::IoResult{0, net::IoError::Other};
    }

    // 读 payload 并反序列化
    std::string buf(len, '\0');
    if (len > 0) {
        r = co_await s.read_exact(buf.data(), len, timeout_ms);
        if (!r.ok()) co_return r;
    }
    if (!out->ParseFromString(buf)) {
        co_return net::IoResult{0, net::IoError::Other};
    }
    co_return net::IoResult{static_cast<size_t>(len), net::IoError::None};
}

coro::Task<bool> WriteFrame(net::TcpStream& s, const RpcFrame& f,
                            int64_t timeout_ms) {
    std::string bytes;
    if (!FrameCodec::Encode(f, &bytes)) co_return false;
    co_return co_await s.write_all(bytes, timeout_ms);
}

}  // namespace rpc
