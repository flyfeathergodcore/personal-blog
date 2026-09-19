#include "tcp/channel.hpp"

#include <utility>

namespace tcp {

Channel::Channel(Stream stream) : stream_(std::move(stream)) {}

coro::Task<IoResult> Channel::Receive(void* buffer, size_t size, int64_t timeout_ms)
{
    co_return co_await stream_.read_some(buffer, size, timeout_ms);
}

coro::Task<IoResult> Channel::ReadUntil(std::string_view delim, std::string& out,
                                        int64_t timeout_ms)
{
    co_return co_await stream_.read_until(delim, out, timeout_ms);
}

coro::Task<IoResult> Channel::ReadExact(size_t size, std::string& out,
                                        int64_t timeout_ms)
{
    co_return co_await stream_.read_exact(size, out, timeout_ms);
}

coro::Task<bool> Channel::Send(std::string_view data, int64_t timeout_ms)
{
    co_return co_await stream_.write_all(data, timeout_ms);
}

coro::Task<bool> Channel::Sendv(std::initializer_list<std::string_view> parts,
                                 int64_t timeout_ms)
{
    co_return co_await stream_.writev_all(parts, timeout_ms);
}

void Channel::Close()
{
    stream_.close();
}

}  // namespace tcp
