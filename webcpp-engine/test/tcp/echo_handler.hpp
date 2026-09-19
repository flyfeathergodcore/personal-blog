#pragma once

#include "coro/task.h"
#include "tcp/channel.hpp"

namespace tcp {

// TCP echo 测试/示例连接处理器。
coro::Task<void> Echo(Channel channel);

}  // namespace tcp
