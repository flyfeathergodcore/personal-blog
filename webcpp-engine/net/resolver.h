// net 层地址解析与连接：resolve() 薄封装 + connect()（协程式非阻塞 TCP 连接）。
// connect 走 getaddrinfo 支持主机名，遍历多地址逐个尝试；每次连接尝试
// 用非阻塞 socket，EINPROGRESS 时在事件循环上等 WRITE 就绪再查 SO_ERROR。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "coro/task.h"
#include "net/tcp_stream.h"

namespace net {

struct Endpoint { std::string host; uint16_t port; };

// getaddrinfo 解析出全部候选端点（host 为数字形式 IP 或主机名）
std::vector<Endpoint> resolve(std::string_view host, uint16_t port);

// 非阻塞 connect：成功返回 TcpStream（fd 已 O_NONBLOCK），失败/超时返回 nullptr
coro::Task<std::unique_ptr<TcpStream>> connect(std::string_view host, uint16_t port,
                                               int64_t timeout_ms = 10000);

}  // namespace net
