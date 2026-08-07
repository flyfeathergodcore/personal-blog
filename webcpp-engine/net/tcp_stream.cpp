#include "net/tcp_stream.h"
#include "coro/awaiter.h"
#include <array>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace net {

// 一次性非阻塞读；返回 -1 且 errno=EAGAIN/EINTR 表示需等事件后重试
static ssize_t try_read(int fd, void* buf, size_t n) {
    for (;;) {
        ssize_t r = ::read(fd, buf, n);
        if (r < 0 && (errno == EINTR)) continue;
        return r;
    }
}

// TcpStream(fd) 是全部连接（accept 产出的服务端连接 + connect 产出的上游连接）
// 的唯一入口。此处设置 TCP_NODELAY：HTTP 响应常为"头 + body"多个小段写入，
// 若不关闭 Nagle 算法，keep-alive 下第二个及之后的响应会撞上对端 delayed ACK
// （Linux 默认 ~40ms），形成经典 40ms 停滞（实测首个请求 0.6ms、后续每个 41ms）。
TcpStream::TcpStream(int fd) : fd_(fd) {
    if (fd_ >= 0) {
        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
TcpStream& TcpStream::operator=(TcpStream&& o) noexcept {
    if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
}

void TcpStream::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

coro::Task<IoResult> TcpStream::read_some(void* buf, size_t n, int64_t timeout_ms) {
    if (fd_ < 0) co_return IoResult{0, IoError::Closed};
    for (;;) {
        ssize_t r = try_read(fd_, buf, n);
        if (r > 0) co_return IoResult{(size_t)r, IoError::None};
        if (r == 0) co_return IoResult{0, IoError::Eof};
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto ready = co_await coro::await_event(fd_, coro::IoPoller::READ, timeout_ms);
            if (ready == coro::Readiness::Timeout)
                co_return IoResult{0, IoError::Timeout};
            continue;   // 就绪后重试；EINTR 已在 try_read 处理
        }
        co_return IoResult{0, IoError::Other};
    }
}

coro::Task<IoResult> TcpStream::read_exact(void* buf, size_t n, int64_t timeout_ms) {
    size_t total = 0;
    while (total < n) {
        auto r = co_await read_some(static_cast<char*>(buf) + total, n - total, timeout_ms);
        if (!r.ok()) co_return r;   // 错误/超时/EOF 原样返回（bytes=本次已读）
        total += r.bytes;
    }
    co_return IoResult{total, IoError::None};
}

coro::Task<bool> TcpStream::write_all(std::string_view data, int64_t timeout_ms) {
    if (fd_ < 0) co_return false;
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t r = ::write(fd_, data.data() + sent, data.size() - sent);
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto ready = co_await coro::await_event(fd_, coro::IoPoller::WRITE, timeout_ms);
            if (ready == coro::Readiness::Timeout) co_return false;
            continue;
        }
        co_return false;   // 对端关闭 / 其他错误
    }
    co_return true;
}

// writev_all：单次系统调用写出多个不连续段（h1 响应头 + 零拷贝 body）。
// 不拷贝用户态数据——iovec 直接指向各段（region / FileCache），
// 把 Send() 的"头一次 write_all + body 一次 write_all"（2 次 syscall）
// 合并为 1 次 writev。部分写/写缓冲满时按段推进并等待可写。
coro::Task<bool> TcpStream::writev_all(std::initializer_list<std::string_view> parts,
                                       int64_t timeout_ms) {
    if (fd_ < 0) co_return false;

    size_t n = parts.size();
    size_t total = 0;
    for (auto p : parts) total += p.size();
    if (total == 0) co_return true;

    // 展开为 iovec（栈数组；段数超 8 才堆分配，当前调用方最多 2 段）。
    std::array<iovec, 8> stack_vecs;
    std::vector<iovec>   dyn_vecs;
    iovec* vecs = stack_vecs.data();
    if (n > stack_vecs.size()) { dyn_vecs.resize(n); vecs = dyn_vecs.data(); }
    {
        size_t i = 0;
        for (auto p : parts) {
            vecs[i].iov_base = const_cast<char*>(p.data());
            vecs[i].iov_len  = p.size();
            ++i;
        }
    }

    size_t sent = 0;
    while (sent < total) {
        // 定位当前待写起点：跳过已完整发送的段，off 为该段内偏移。
        size_t first = 0, off = sent;
        while (first < n && off >= vecs[first].iov_len) {
            off -= vecs[first].iov_len;
            ++first;
        }
        if (first == n) break;   // 防御：sent 已到 total

        // 构造剩余段的 iovec（段数 ≤ n：当前段余量 + 后续整段）。
        std::array<iovec, 8> suf_stack;
        std::vector<iovec>   suf_dyn;
        iovec* suf = suf_stack.data();
        if (n > suf_stack.size()) { suf_dyn.resize(n); suf = suf_dyn.data(); }
        size_t cnt = 0;
        if (off > 0) {
            suf[cnt].iov_base = static_cast<char*>(vecs[first].iov_base) + off;
            suf[cnt].iov_len  = vecs[first].iov_len - off;
            ++cnt;
            ++first;
        }
        while (first < n) {
            suf[cnt] = vecs[first];
            ++cnt; ++first;
        }

        ssize_t r = ::writev(fd_, suf, static_cast<int>(cnt));
        if (r > 0) { sent += static_cast<size_t>(r); continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto ready = co_await coro::await_event(fd_, coro::IoPoller::WRITE, timeout_ms);
            if (ready == coro::Readiness::Timeout) co_return false;
            continue;
        }
        co_return false;   // 对端关闭 / 其他错误
    }
    co_return true;
}

}  // namespace net
