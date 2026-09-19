/*
    channel.hpp
    TCP 连接通道：对 TCP 连接的协程式封装，提供发送和接收操作。
    设计：
        - 构造时传入 Stream，析构时关闭 Stream
        - Receive() / Send() / Sendv() 提供协程式读写，挂起在事件循环上等待就绪事件
        - IsOpen() / Fd() / Close() 提供连接状态管理
*/

#pragma once

#include "coro/task.h"
#include "tcp/stream.hpp"

#include <cstddef>
#include <initializer_list>
#include <string_view>
#include <string>

namespace tcp {

class Listener;

class Channel {
public:
    Channel() = default;
    explicit Channel(Stream stream);

    Channel(Channel&&) noexcept = default;
    Channel& operator=(Channel&&) noexcept = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    coro::Task<IoResult> Receive(void* buffer, size_t size, int64_t timeout_ms = -1);
    coro::Task<IoResult> ReadUntil(std::string_view delim, std::string& out,
                                   int64_t timeout_ms = -1);
    coro::Task<IoResult> ReadExact(size_t size, std::string& out,
                                   int64_t timeout_ms = -1);
    coro::Task<bool> Send(std::string_view data, int64_t timeout_ms = -1);
    coro::Task<bool> Sendv(std::initializer_list<std::string_view> parts,
                           int64_t timeout_ms = -1);

    bool IsOpen() const { return stream_.is_open(); }
    int Fd() const { return stream_.fd(); }
    std::string_view Buffered() const { return stream_.buffered(); }
    std::string TakeBuffered() { return stream_.take_buffered(); }
    void Close();

private:
    friend class Listener;
    Stream stream_;
};

}  // namespace tcp
