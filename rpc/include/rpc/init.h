// RPC 框架初始化：屏蔽 SIGPIPE。
// TcpStream::write_all 走裸 ::write（无 MSG_NOSIGNAL），对端关闭后写会触发
// SIGPIPE（默认终止进程）。任何使用本框架的程序在 main 入口调用 rpc::Init()。
#pragma once

#include <csignal>

namespace rpc {

// 屏蔽 SIGPIPE（进程级、幂等）：写已关闭连接返回 EPIPE 而非发信号
inline void Init() {
    static bool done = false;
    if (!done) {
        ::signal(SIGPIPE, SIG_IGN);
        done = true;
    }
}

}  // namespace rpc
