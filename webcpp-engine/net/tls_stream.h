// net 层 TLS 流：封装 net::TcpStream + SSL（服务端模式），协程式非阻塞握手/读写。
// 设计：底层 fd 设为 O_NONBLOCK；SSL_accept/read/write 返回 WANT_READ/WANT_WRITE
// 时在事件循环上等待对应就绪事件后重试，不阻塞线程。
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include <openssl/ssl.h>

#include "coro/task.h"
#include "net/tcp_stream.h"

namespace net {

class TlsStream {
public:
    // 构造：包装底层连接与 TLS 上下文（服务端模式）
    // 参数：tcp - 底层连接（接管所有权）；ctx - TLS 上下文
    TlsStream(TcpStream tcp, SSL_CTX* ctx);
    // 析构：关闭 TLS 连接与底层 fd
    ~TlsStream();
    // 移动构造/赋值：接管底层连接与 SSL 对象
    TlsStream(TlsStream&&) noexcept;
    TlsStream& operator=(TlsStream&&) noexcept;
    // 禁止拷贝
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // 服务端 TLS 握手（SSL_accept 循环），成功返回 IoError::None
    coro::Task<IoResult> handshake(int64_t timeout_ms = 10000);
    // 解密读取一帧，语义与 TcpStream::read_some 一致（Eof=对端 close_notify）
    coro::Task<IoResult> read_some(void* buf, size_t n, int64_t timeout_ms = -1);
    // 全量加密写出（内部循环处理部分写），成功返回 true
    coro::Task<bool>     write_all(std::string_view data, int64_t timeout_ms = -1);
    // 逐段 write_all（TLS 无系统 writev：SSL_write 需连续缓冲且加密已是瓶颈，
    // 合并无 syscall 收益；此接口仅为与 TcpStream 的模板接口对齐）。
    coro::Task<bool>     writev_all(std::initializer_list<std::string_view> parts,
                                    int64_t timeout_ms = -1);

    // 底层连接是否有效
    bool is_open() const { return tcp_.is_open(); }
    // 关闭 TLS 连接与底层 fd（幂等）
    void close();                     // 幂等；尽力发 close_notify 后释放
    // 返回底层 fd
    int  fd() const { return tcp_.fd(); }
    // 返回底层 SSL*（未初始化为 nullptr）
    SSL* native_handle() const { return ssl_; }

private:
    TcpStream tcp_;
    SSL_CTX*  ctx_ = nullptr;
    SSL*      ssl_ = nullptr;
};

}  // namespace net
