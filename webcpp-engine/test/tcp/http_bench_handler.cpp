#include "test/tcp/http_bench_handler.hpp"

#include <string>
#include <string_view>

namespace tcp {
namespace {

constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "OK";

}  // namespace

coro::Task<void> HttpBench(Channel channel)
{
    /*
    1. 按 HTTP 请求头终止符读取，Stream 内部缓冲确保 pipelined 数据不会丢失
    2. 不解析方法、路径和请求体，收到完整头部即返回固定 200 响应
    3. 保持连接，下一轮继续读取，用于稳定测量 keep-alive QPS
    */
    std::string request;
    while (true)
    {
        const auto read = co_await channel.ReadUntil("\r\n\r\n", request);
        if (!read.ok())
            break;
        if (!(co_await channel.Send(kHttpResponse)))
            break;
    }
    co_return;
}

}  // namespace tcp
