#include "test/tcp/echo_handler.hpp"

namespace tcp {

coro::Task<void> Echo(Channel channel)
{
    char buffer[4096];
    while (true) {
        const auto read = co_await channel.Receive(buffer, sizeof(buffer));
        if (!read.ok()) break;
        if (!(co_await channel.Send({buffer, read.bytes}))) break;
    }
    co_return;
}

}  // namespace tcp
