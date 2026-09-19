#include "tcp/stream.hpp"
#include "tcp/tcp_log.hpp"
#include "coro/awaiter.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace tcp
{

    static ssize_t try_read(int fd, void *buf, size_t n)
    {
        /*
        一次性非阻塞读取数据。返回r>0表示读取到r字节，r=0表示EOF，r<0表示错误（errno可判断类型）。
        */
        for (;;)
        {
            ssize_t r = ::read(fd, buf, n);
            if (r < 0 && (errno == EINTR))
                continue;
            return r;
        }
    }

    Stream::Stream(int fd) : fd_(fd)
    {
        /*
        1.判定 fd_ 是否有效（>=0），若无效则不设置 TCP_NODELAY
        2.设置IPPROTO_TCP选项 TCP_NODELAY，禁用 Nagle 算法
        3.若设置失败，记录错误日志，包含 fd_ 和错误信息

        解释：
        - TCP_NODELAY：禁用 Nagle 算法，减少延迟，适用于需要低延迟的应用场景
        */
        if (fd_ >= 0)
        {
            int one = 1;
            if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
            {
                Log(LogLevel::Error, "TCP_STREAM",
                    "setsockopt(TCP_NODELAY) failed for fd " +
                        std::to_string(fd_) + ": " + std::strerror(errno));
            }
        }
    }

    Stream::~Stream() { close(); }

    Stream::Stream(Stream &&o) noexcept
        : fd_(o.fd_), read_buffer_(std::move(o.read_buffer_)), read_pos_(o.read_pos_)
    {
        // 移动构造：接管 o 的 fd，o 置空（fd_=-1）
        o.fd_ = -1;
        o.read_buffer_.clear();
        o.read_pos_ = 0;
    }

    Stream &Stream::operator=(Stream &&o) noexcept
    {
        // 移动赋值：先关闭自身旧 fd，再接管 o 的 fd
        if (this != &o)
        {
            close();
            fd_ = o.fd_;
            read_buffer_ = std::move(o.read_buffer_);
            read_pos_ = o.read_pos_;
            o.fd_ = -1;
            o.read_buffer_.clear();
            o.read_pos_ = 0;
        }
        return *this;
    }


    void Stream::close()
    {
        // 关闭fd（幂等）：若 fd_ >= 0，则调用 ::close(fd_)，并将 fd_ 置为 -1
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        read_buffer_.clear();
        read_pos_ = 0;
    }

    // 非阻塞读取尽可能多的数据（单次 read，返回 0 字节表示 EOF）
    // 参数：buf - 接收缓冲区；n - 缓冲容量；timeout_ms - 等待超时（毫秒，<0 无限）
    coro::Task<IoResult> Stream::read_raw(void *buf, size_t n, int64_t timeout_ms)
    {   
        /*
        1. 检查 fd_ 是否有效（>=0），若无效则返回 IoResult{0, IoError::Closed}
        2. 循环尝试读取数据：
            a. 调用 try_read(fd_, buf, n) 尝试读取数据
            b. 若读取成功（r > 0），返回 IoResult{(size_t)r, IoError::None}
            c. 若读取到 EOF（r == 0），返回 IoResult{0, IoError::Eof}
            d. 若读取失败且 errno 为 EAGAIN 或 EWOULDBLOCK，表示当前无数据可读：
                i. 使用 co_await 等待 fd_ 可读事件，带超时参数
                ii. 若等待超时，返回 IoResult{0, IoError::Timeout}
                iii. 若等待成功，继续循环尝试读取数据
            e. 若读取失败且 errno 为其他值，返回 IoResult{0, IoError::Other}
        3. 该函数是一个协程，使用 co_await 来挂起等待 fd_ 可读事件，支持超时处理
        4. 返回的 IoResult 包含实际读取的字节数和错误类型，便于调用方处理
        5. 该函数适用于非阻塞 I/O 场景，结合协程和事件循环，可以实现高效的异步读取操作

        参数：
        - buf: 指向接收数据的缓冲区
        - n: 缓冲区的大小（字节数）
        - timeout_ms: 等待可读事件的超时时间（毫秒），<0 表示无限等待
        返回值：
        - IoResult: 包含实际读取的字节数和错误类型
        */
        if (fd_ < 0)
            co_return IoResult{0, IoError::Closed};
        if (n == 0)
            co_return IoResult{0, IoError::None};
        for (;;)
        {
            ssize_t r = try_read(fd_, buf, n);
            if (r > 0)
                co_return IoResult{(size_t)r, IoError::None};
            if (r == 0)
                co_return IoResult{0, IoError::Eof};
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                auto ready = co_await coro::await_event(fd_, coro::IoPoller::READ, timeout_ms);
                if (ready == coro::Readiness::Timeout)
                    co_return IoResult{0, IoError::Timeout};
                continue;
            }
            co_return IoResult{0, IoError::Other};
        }
    }

    coro::Task<IoResult> Stream::read_some(void *buf, size_t n, int64_t timeout_ms)
    {
        if (fd_ < 0)
            co_return IoResult{0, IoError::Closed};
        if (n == 0)
            co_return IoResult{0, IoError::None};

        const size_t available = read_buffer_.size() - read_pos_;
        if (available > 0)
        {
            const size_t take = std::min(available, n);
            std::memcpy(buf, read_buffer_.data() + read_pos_, take);
            read_pos_ += take;
            compact_buffer();
            co_return IoResult{take, IoError::None};
        }
        co_return co_await read_raw(buf, n, timeout_ms);
    }


    
    coro::Task<IoResult> Stream::read_exact(void *buf, size_t n, int64_t timeout_ms)
    {
        /*
        1. 设置一个变量 total 用于记录已读取的字节数，初始值为 0
        2. 循环读取数据，直到 total 达到 n：
            a. 调用 read_some(buf + total, n - total, timeout_ms) �协程读取剩余字节数
            b. 如果 read_some 返回的 IoResult 不 ok（即发生错误、超时或 EOF），则直接返回该 IoResult，包含已读取的字节数
            c. 如果 read_some 成功读取数据，则将返回的 bytes 累加到 total
        3. 当 total 达到 n 时，表示已成功读取到指定字节数，返回 IoResult{total, IoError::None}
        4. 该函数是一个协程，使用 co_await 来挂起等待 read_some 的结果，支持超时处理
        5. 返回的 IoResult 包含实际读取的字节数和错误类型，便于调用方处理
        */
        size_t total = 0;
        while (total < n)
        {
            auto r = co_await read_some(static_cast<char *>(buf) + total, n - total, timeout_ms);
            if (!r.ok())
                co_return r; // 错误/超时/EOF 原样返回（bytes=本次已读）
            total += r.bytes;
        }
        co_return IoResult{total, IoError::None};
    }

    coro::Task<IoResult> Stream::read_until(std::string_view delim, std::string& out,
                                            int64_t timeout_ms)
    {
        out.clear();
        if (fd_ < 0)
            co_return IoResult{0, IoError::Closed};
        if (delim.empty())
            co_return IoResult{0, IoError::Other};

        for (;;)
        {
            const size_t hit = read_buffer_.find(delim, read_pos_);
            if (hit != std::string::npos)
            {
                out.append(read_buffer_.data() + read_pos_, hit - read_pos_);
                read_pos_ = hit + delim.size();
                compact_buffer();
                co_return IoResult{out.size(), IoError::None};
            }

            const size_t available = read_buffer_.size() - read_pos_;
            if (available > delim.size() - 1)
            {
                const size_t move = available - (delim.size() - 1);
                out.append(read_buffer_.data() + read_pos_, move);
                read_pos_ += move;
                compact_buffer();
            }

            char tmp[4096];
            auto r = co_await read_raw(tmp, sizeof(tmp), timeout_ms);
            if (!r.ok())
                co_return IoResult{out.size(), r.err};
            read_buffer_.append(tmp, r.bytes);
        }
    }

    coro::Task<IoResult> Stream::read_exact(size_t n, std::string& out,
                                            int64_t timeout_ms)
    {
        out.clear();
        out.reserve(n);
        while (out.size() < n)
        {
            char tmp[4096];
            const size_t want = std::min(n - out.size(), sizeof(tmp));
            auto r = co_await read_some(tmp, want, timeout_ms);
            if (!r.ok())
            {
                co_return IoResult{out.size(), r.err};
            }
            out.append(tmp, r.bytes);
        }
        co_return IoResult{n, IoError::None};
    }

    std::string_view Stream::buffered() const
    {
        return std::string_view(read_buffer_).substr(read_pos_);
    }

    std::string Stream::take_buffered()
    {
        std::string out(read_buffer_.data() + read_pos_, read_buffer_.size() - read_pos_);
        read_buffer_.clear();
        read_pos_ = 0;
        return out;
    }

    void Stream::reserve_read_buffer(size_t capacity)
    {
        if (capacity > read_buffer_.capacity())
            read_buffer_.reserve(capacity);
    }

    void Stream::compact_buffer()
    {
        if (read_pos_ == read_buffer_.size())
        {
            read_buffer_.clear();
            read_pos_ = 0;
        }
        else if (read_pos_ >= 4096 && read_pos_ * 2 >= read_buffer_.size())
        {
            read_buffer_.erase(0, read_pos_);
            read_pos_ = 0;
        }
    }

    // 全量写出 data（内部循环处理部分写/写满时等待可写）
    // 参数：data - 待写数据；timeout_ms - 写超时（毫秒，<0 无限）。全部写完返回 true
    coro::Task<bool> Stream::write_all(std::string_view data, int64_t timeout_ms)
    {
        /*
        1. 检查 fd_ 是否有效（>=0），若无效则返回 false
        2. 设置一个变量 sent 用于记录已写入的字节数，初始值为 0
        3. 循环写入数据，直到 sent 达到 data.size()：
            a. 调用 ::write(fd_, data.data() + sent, data.size() - sent) 尝试写入剩余字节数
            b. 如果写入成功（r > 0），将返回的字节数累加到 sent，并继续循环
            c. 如果写入失败且 errno 为 EINTR，表示被中断，继续循环尝试写入
            d. 如果写入失败且 errno 为 EAGAIN 或 EWOULDBLOCK，表示当前无法写入：
                i. 使用 co_await 等待 fd_ 可写事件，带超时参数
                ii. 若等待超时，返回 false
                iii. 若等待成功，继续循环尝试写入
            e. 如果写入失败且 errno 为其他值，返回 false，表示对端关闭或发生其他错误
        4. 当 sent 达到 data.size() 时，表示已成功写入所有数据，返回 true
        5. 该函数是一个协程，使用 co_await 来挂起等待 fd_ 可写事件，支持超时处理
        6. 返回值为 bool，表示是否成功写入所有数据，便于调用方处理
        7. 该函数适用于非阻塞 I/O 场景，结合协程和事件循环，可以实现高效的异步写入操作
        8. 注意：在写入过程中，如果发生部分写入，函数会继续尝试写入剩余数据，直到全部写完或发生错误/超时
        */
        if (fd_ < 0)
            co_return false;
        size_t sent = 0;
        while (sent < data.size())
        {
            ssize_t r = ::write(fd_, data.data() + sent, data.size() - sent);
            if (r > 0)
            {
                sent += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                auto ready = co_await coro::await_event(fd_, coro::IoPoller::WRITE, timeout_ms);
                if (ready == coro::Readiness::Timeout)
                    co_return false;
                continue;
            }
            co_return false;
        }
        co_return true;
    }


    coro::Task<bool> Stream::writev_all(std::initializer_list<std::string_view> parts,
                                        int64_t timeout_ms)
    {
        /*
        输入示例：
            - stream.writev_all({header,body,trailer});

        算法：
        1. 检查 fd_ 是否有效（>=0），若无效则返回 false
        2. 计算总字节数 total，如果 total 为 0，则直接返回 true，表示无需写入
        3. 将 parts 展开为 iovec 数组(iovec 散乱的内存块)，使用栈数组array或堆数组vector，根据段数决定
        4. 设置一个变量 sent 用于记录已写入的字节数，初始值为 0
        5. 循环写入数据，直到 sent 达到 total：
            a. 定位当前待写起点：跳过已完整发送的段，计算 off 为该段内偏移
            b. 构造剩余段的 iovec 数组（当前段余量 + 后续整段）
            c. 调用 ::writev(fd_, suf, cnt) 尝试写入剩余段
            d. 如果写入成功（r > 0），将返回的字节数累加到 sent，并继续循环
            e. 如果写入失败且 errno 为 EINTR，表示被中断，继续循环尝试写入
            f. 如果写入失败且 errno 为 EAGAIN 或 EWOULDBLOCK，表示当前无法写入：
                i. 使用 co_await 等待 fd_ 可写事件，带超时参数
                ii. 若等待超时，返回 false
                iii. 若等待成功，继续循环尝试写入
            g. 如果写入失败且 errno 为其他值，返回 false，表示对端关闭或发生其他错误
        6. 当 sent 达到 total 时，表示已成功写入所有数据，返回 true
        7. 该函数是一个协程，使用 co_await 来挂起等待 fd_ 可写事件，支持超时处理
        8. 返回值为 bool，表示是否成功写入所有数据，便于调用方处理
        */
        if (fd_ < 0)
            co_return false;

        size_t n = parts.size();
        size_t total = 0;
        for (auto p : parts)
            total += p.size();
        if (total == 0)
            co_return true;

        // 展开为 iovec（栈数组；段数超 8 才堆分配）。后续部分写直接
        // 原地推进当前 iovec，避免每轮重建一份“剩余分段”数组。
        std::array<iovec, 8> stack_vecs;
        std::vector<iovec> dyn_vecs;
        iovec *vecs = stack_vecs.data();
        if (n > stack_vecs.size())
        {
            dyn_vecs.resize(n);
            vecs = dyn_vecs.data();
        }
        {
            size_t i = 0;
            for (auto p : parts)
            {
                vecs[i].iov_base = const_cast<char *>(p.data());
                vecs[i].iov_len = p.size();
                ++i;
            }
        }

        size_t sent = 0;
        size_t first = 0;
        while (sent < total)
        {
            // 跳过空段；writev 只接收当前尚未发送的连续 iovec 后缀。
            while (first < n && vecs[first].iov_len == 0)
            {
                ++first;
            }
            if (first == n)
                co_return false; // total/sent 不一致，防御性失败

            ssize_t r = ::writev(fd_, vecs + first, static_cast<int>(n - first));
            if (r > 0)
            {
                size_t advanced = static_cast<size_t>(r);
                sent += advanced;

                // 消费完整段；若结束于某段中间，只移动该段的起点和长度。
                while (first < n && advanced >= vecs[first].iov_len)
                {
                    advanced -= vecs[first].iov_len;
                    vecs[first].iov_len = 0;
                    ++first;
                }
                if (first < n && advanced > 0)
                {
                    vecs[first].iov_base =
                        static_cast<char *>(vecs[first].iov_base) + advanced;
                    vecs[first].iov_len -= advanced;
                }
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                auto ready = co_await coro::await_event(fd_, coro::IoPoller::WRITE, timeout_ms);
                if (ready == coro::Readiness::Timeout)
                    co_return false;
                continue;
            }
            co_return false; // 对端关闭 / 其他错误
        }
        co_return true;
    }

} // namespace tcp
