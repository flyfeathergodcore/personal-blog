#pragma once

#include "coro/task.h"
#include "tcp/channel.hpp"

namespace tcp {

// 固定 HTTP/1.1 200 响应，用于 TCP 服务的 keep-alive 吞吐测试。
coro::Task<void> HttpBench(Channel channel);

}  // namespace tcp
