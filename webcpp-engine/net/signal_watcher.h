// net 层信号监听：SignalWatcher 封装（signalfd + 协程 wait）。
// 设计：init 把信号加入 sigset 并 SIG_BLOCK（必须在 signalfd 创建前 block，
// 否则信号走默认动作），创建 signalfd 后信号转为 fd 可读事件；
// wait() 在事件循环上等 READ 就绪后读取 signalfd_siginfo，返回信号号。
#pragma once

#include <vector>

#include "coro/task.h"

namespace net {

class SignalWatcher {
public:
    bool init(const std::vector<int>& sigs);   // signalfd；需调用前在进程内 block 这些信号
    coro::Task<int> wait();
    void close();
    int fd() const { return fd_; }
private:
    int fd_ = -1;
};

}  // namespace net
