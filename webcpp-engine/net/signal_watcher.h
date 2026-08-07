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
    // 初始化 signalfd 并 block 指定信号（需在创建前 block，否则信号走默认动作）
    // 参数：sigs - 需监听的信号集合
    bool init(const std::vector<int>& sigs);   // signalfd；需调用前在进程内 block 这些信号
    // 协程等待信号并返回信号号；失败返回 -1
    coro::Task<int> wait();
    // 关闭 signalfd（幂等）
    void close();
    // 返回 signalfd 描述符
    int fd() const { return fd_; }
private:
    int fd_ = -1;
};

}  // namespace net
