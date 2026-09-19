// net 层 TLS 流实现：非阻塞握手/读写，WANT_READ/WANT_WRITE 时挂起到事件循环。
#include "net/tls_stream.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <unistd.h>

#include "coro/awaiter.h"

namespace net {

namespace {

// 单调时钟毫秒（用于超时截止计算）
int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 计算剩余超时：timeout_ms < 0 返回 -1（无限）；已超时返回 0
int64_t remaining_ms(int64_t deadline, int64_t timeout_ms)
{
    if (timeout_ms < 0) return -1;
    int64_t rem = deadline - now_ms();
    return rem <= 0 ? 0 : rem;
}

}  // namespace

// 构造：包装底层 tcp::Stream 与 SSL_CTX，创建服务端模式 SSL 对象并确保 fd 非阻塞
// 参数：tcp - 底层连接（接管所有权）；ctx - TLS 上下文
TlsStream::TlsStream(tcp::Stream tcp, SSL_CTX* ctx)
    : tcp_(std::move(tcp)), ctx_(ctx)
{
    if (!ctx_ || tcp_.fd() < 0) return;
    ssl_ = SSL_new(ctx_);
    if (!ssl_) return;
    SSL_set_fd(ssl_, tcp_.fd());       // 底层 socket BIO 为 BIO_NOCLOSE，不接管 fd 所有权
    SSL_set_accept_state(ssl_);        // 服务端模式
    // 允许部分写 + 写缓冲可移动，配合 SSL_write 循环
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    // 确保底层 fd 非阻塞（调用方可能未设置）
    int fl = fcntl(tcp_.fd(), F_GETFL, 0);
    if (fl >= 0) fcntl(tcp_.fd(), F_SETFL, fl | O_NONBLOCK);
}

// 析构：关闭 TLS 连接与底层 fd
TlsStream::~TlsStream() { close(); }

// 移动构造：接管 o 的底层连接与 SSL 对象，o 置空
TlsStream::TlsStream(TlsStream&& o) noexcept
    : tcp_(std::move(o.tcp_)), ctx_(o.ctx_), ssl_(o.ssl_)
{
    o.ctx_ = nullptr;
    o.ssl_ = nullptr;
}

// 移动赋值：先关闭自身，再接管 o 的底层连接与 SSL 对象
TlsStream& TlsStream::operator=(TlsStream&& o) noexcept
{
    if (this != &o) {
        close();
        tcp_ = std::move(o.tcp_);
        ctx_ = o.ctx_;
        ssl_ = o.ssl_;
        o.ctx_ = nullptr;
        o.ssl_ = nullptr;
    }
    return *this;
}

// 关闭 TLS 连接：尽力发送 close_notify 并释放 SSL，随后关闭底层 fd（幂等）
void TlsStream::close()
{
    if (ssl_) {
        SSL_shutdown(ssl_);   // 尽力发送 close_notify，非阻塞下忽略结果
        SSL_free(ssl_);       // BIO 为 BIO_NOCLOSE，不关闭 fd
        ssl_ = nullptr;
    }
    tcp_.close();
}

// 服务端 TLS 握手（SSL_accept 循环），WANT_READ/WANT_WRITE 时挂起等待事件
// 参数：timeout_ms - 握手超时（毫秒，<0 无限）。成功返回 {0, None}
coro::Task<tcp::IoResult> TlsStream::handshake(int64_t timeout_ms)
{
    if (!ssl_ || !tcp_.is_open()) co_return tcp::IoResult{0, tcp::IoError::Closed};
    const int64_t deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : 0;
    for (;;) {
        int r = SSL_accept(ssl_);
        if (r == 1) co_return tcp::IoResult{0, tcp::IoError::None};   // 握手完成

        int err = SSL_get_error(ssl_, r);
        coro::IoPoller::Event ev;
        if (err == SSL_ERROR_WANT_READ) {
            ev = coro::IoPoller::READ;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            ev = coro::IoPoller::WRITE;
        } else if (err == SSL_ERROR_SYSCALL && errno == EAGAIN) {
            ev = coro::IoPoller::READ;   // 底层数据未就绪，等可读重试
        } else {
            // 对端在握手完成前关闭 / 协议错误
            if (err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL)
                co_return tcp::IoResult{0, tcp::IoError::Eof};
            co_return tcp::IoResult{0, tcp::IoError::Other};
        }

        int64_t rem = remaining_ms(deadline, timeout_ms);
        if (rem == 0) co_return tcp::IoResult{0, tcp::IoError::Timeout};
        auto ready = co_await coro::await_event(tcp_.fd(), ev, rem);
        if (ready == coro::Readiness::Timeout)
            co_return tcp::IoResult{0, tcp::IoError::Timeout};
    }
}

// 解密读取一帧数据，语义与 tcp::Stream::read_some 一致（Eof=对端 close_notify）
// 参数：buf - 接收缓冲区；n - 缓冲容量；timeout_ms - 等待超时（毫秒，<0 无限）
coro::Task<tcp::IoResult> TlsStream::read_some(void* buf, size_t n, int64_t timeout_ms)
{
    if (!ssl_ || !tcp_.is_open()) co_return tcp::IoResult{0, tcp::IoError::Closed};
    if (n == 0) co_return tcp::IoResult{0, tcp::IoError::None};
    const int64_t deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : 0;
    const int max_n = static_cast<int>(std::min<size_t>(n, INT_MAX));
    for (;;) {
        int r = SSL_read(ssl_, buf, max_n);
        if (r > 0) co_return tcp::IoResult{static_cast<size_t>(r), tcp::IoError::None};

        int err = SSL_get_error(ssl_, r);
        coro::IoPoller::Event ev;
        if (err == SSL_ERROR_WANT_READ) {
            ev = coro::IoPoller::READ;
        } else if (err == SSL_ERROR_WANT_WRITE) {
            ev = coro::IoPoller::WRITE;   // 重协商等场景
        } else if (err == SSL_ERROR_SYSCALL && errno == EAGAIN) {
            ev = coro::IoPoller::READ;
        } else {
            if (err == SSL_ERROR_ZERO_RETURN) co_return tcp::IoResult{0, tcp::IoError::Eof};
            if (err == SSL_ERROR_SYSCALL && r == 0) co_return tcp::IoResult{0, tcp::IoError::Eof};
            co_return tcp::IoResult{0, tcp::IoError::Other};
        }

        int64_t rem = remaining_ms(deadline, timeout_ms);
        if (rem == 0) co_return tcp::IoResult{0, tcp::IoError::Timeout};
        auto ready = co_await coro::await_event(tcp_.fd(), ev, rem);
        if (ready == coro::Readiness::Timeout)
            co_return tcp::IoResult{0, tcp::IoError::Timeout};
    }
}

// 全量加密写出 data（内部循环处理部分写/等待可写）
// 参数：data - 待写数据；timeout_ms - 写超时（毫秒，<0 无限）。全部写完返回 true
coro::Task<bool> TlsStream::write_all(std::string_view data, int64_t timeout_ms)
{
    if (!ssl_ || !tcp_.is_open()) co_return false;
    const int64_t deadline = (timeout_ms >= 0) ? now_ms() + timeout_ms : 0;
    size_t sent = 0;
    while (sent < data.size()) {
        int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, INT_MAX));
        int r = SSL_write(ssl_, data.data() + sent, chunk);
        if (r > 0) { sent += static_cast<size_t>(r); continue; }

        int err = SSL_get_error(ssl_, r);
        coro::IoPoller::Event ev;
        if (err == SSL_ERROR_WANT_WRITE) {
            ev = coro::IoPoller::WRITE;
        } else if (err == SSL_ERROR_WANT_READ) {
            ev = coro::IoPoller::READ;   // 重协商等场景
        } else if (err == SSL_ERROR_SYSCALL && errno == EAGAIN) {
            ev = coro::IoPoller::WRITE;
        } else {
            co_return false;   // 对端关闭 / 其他错误
        }

        int64_t rem = remaining_ms(deadline, timeout_ms);
        if (rem == 0) co_return false;
        auto ready = co_await coro::await_event(tcp_.fd(), ev, rem);
        if (ready == coro::Readiness::Timeout) co_return false;
    }
    co_return true;
}

// 逐段调用 write_all 全量写出（TLS 无系统 writev 收益，仅对齐 tcp::Stream 接口）
// 参数：parts - 待写段列表；timeout_ms - 写超时（毫秒，<0 无限）。全部写完返回 true
coro::Task<bool> TlsStream::writev_all(std::initializer_list<std::string_view> parts,
                                       int64_t timeout_ms)
{
    for (auto p : parts)
        if (!(co_await write_all(p, timeout_ms)))
            co_return false;
    co_return true;
}

}  // namespace net
