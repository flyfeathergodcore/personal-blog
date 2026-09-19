// TCP 传输层地址解析与非阻塞连接。
// connect 走 getaddrinfo 支持主机名，遍历多地址逐个尝试；每次连接尝试
// 用非阻塞 socket，EINPROGRESS 时在事件循环上等 WRITE 就绪再查 SO_ERROR。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "coro/task.h"
#include "tcp/stream.hpp"

namespace tcp {

struct Endpoint { std::string host; uint16_t port; };

// getaddrinfo 解析出全部候选端点（host 为数字形式 IP 或主机名）
std::vector<Endpoint> Resolve(std::string_view host, uint16_t port);

// 非阻塞 connect：成功返回 Stream（fd 已 O_NONBLOCK），失败/超时返回 nullptr。
coro::Task<std::unique_ptr<Stream>> Connect(std::string_view host, uint16_t port,
                                            int64_t timeout_ms = 10000);

}  // namespace tcp
