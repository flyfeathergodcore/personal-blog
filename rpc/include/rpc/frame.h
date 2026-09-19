// rpc 帧编解码：长度前缀帧 [4 字节大端长度][RpcFrame protobuf 序列化]。
// 编解码为同步纯函数；网络读写封装成协程（依赖 net::TcpStream 的 read_exact/write_all）。
#pragma once

#include <cstdint>
#include <string>

#include "coro/task.h"
#include "net/tcp_stream.h"
#include "rpc_meta.pb.h"

namespace rpc {

// 单帧最大长度：防御恶意/损坏的长度头（16MB）
inline constexpr uint32_t kMaxFrameLen = 16u * 1024u * 1024u;

// 帧编解码工具：长度前缀大端 + protobuf 负载
class FrameCodec {
public:
    // 把帧编码为 [4B 大端长度][序列化负载]；失败（序列化异常）返回 false
    // 参数：f - 帧消息；out - 输出字节串
    static bool Encode(const RpcFrame& f, std::string* out);

    // 从 4 字节长度头解析长度并做上限校验；合法返回 true 并写入 len
    // 参数：hdr - 4 字节长度头（大端）；len - 输出负载长度
    static bool TryDecodeHeader(const uint8_t hdr[4], uint32_t* len);
};

// 可恢复帧读取器：把"读一帧"拆成可中断的多次读，跨轮询超时保留部分已读状态，
// 保证读循环用有限超时轮询（用于感知对端关闭——macOS kqueue 不会唤醒已关闭
// fd 上已注册的读等待）不会打断一帧的完整性。
class FrameReader {
public:
    // 读完整一帧。成功 IoResult{负载长度, None}；超时保留部分已读，下次续读；
    // EOF/断开/非法帧返回对应错误。
    // 参数：s - 底层流；out - 输出帧；timeout_ms - 每轮读的等待上限（-1 无限）
    coro::Task<net::IoResult> Read(net::TcpStream& s, RpcFrame* out, int64_t timeout_ms);

    // 复位状态（新连接前调用，清空上次连接可能残留的部分帧）
    void Reset() {
        stage_ = Stage::kHeader;
        hdr_got_ = 0;
        want_ = 0;
        body_got_ = 0;
        body_.clear();
    }

private:
    enum class Stage { kHeader, kBody } stage_ = Stage::kHeader;
    uint8_t hdr_[4] = {0, 0, 0, 0};  // 长度头缓冲
    std::size_t hdr_got_ = 0;         // 头已读字节
    std::uint32_t want_ = 0;          // 头解析出的帧长
    std::string body_;                // 负载缓冲
    std::size_t body_got_ = 0;        // 负载已读字节
};

// 协程式读一帧：先读 4 字节头，校验长度，再读 payload 并反序列化。
// 成功 IoResult{bytes=负载长度, None}；EOF/超时/非法帧返回对应错误
// 参数：s - 底层流；out - 输出帧；timeout_ms - 每次读超时（-1 无限）
coro::Task<net::IoResult> ReadFrame(net::TcpStream& s, RpcFrame* out,
                                    int64_t timeout_ms = -1);

// 协程式写一帧：编码后整体写出（调用方需自行保证写互斥，见 write_lock.h）
// 参数：s - 底层流；f - 帧；timeout_ms - 写超时
coro::Task<bool> WriteFrame(net::TcpStream& s, const RpcFrame& f,
                            int64_t timeout_ms = -1);

}  // namespace rpc
