// SignalWatcher：信号等待封装，跨平台双实现，接口完全一致：
//   - Linux：signalfd（把信号转成 fd 可读事件，协程在事件循环上等待）
//   - macOS/BSD：无 signalfd，用 self-pipe（自管道）——信号 handler 向管道写
//     一个字节信号号，协程等待管道读端可读，读回信号号。语义等价。
#include "net/signal_watcher.h"

#include "coro/awaiter.h"

#include <cerrno>
#include <csignal>
#include <unistd.h>

#ifdef __linux__
#include <sys/signalfd.h>
#else
#include <fcntl.h>
#endif

namespace net {

#ifdef __linux__

// 初始化 signalfd：block 指定信号并创建 signalfd（非阻塞 + CLOEXEC）
// 参数：sigs - 需监听的信号集合。成功返回 true（fd_ 有效）
bool SignalWatcher::init(const std::vector<int>& sigs) {
    sigset_t set;
    sigemptyset(&set);
    for (int s : sigs) sigaddset(&set, s);
    sigprocmask(SIG_BLOCK, &set, nullptr);          // 必须在 signalfd 创建前 block
    fd_ = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    return fd_ >= 0;
}

// 协程等待下一个信号：READ 就绪后读取 signalfd_siginfo 返回信号号
// 参数：无。返回信号编号；失败/非预期返回 -1
coro::Task<int> SignalWatcher::wait() {
    struct signalfd_siginfo info;
    for (;;) {
        auto r = co_await coro::await_readable(fd_);
        (void)r;
        ssize_t n = ::read(fd_, &info, sizeof(info));
        if (n == (ssize_t)sizeof(info)) co_return (int)info.ssi_signo;
        if (n < 0 && errno == EAGAIN) continue;
        co_return -1;
    }
}

// 关闭 signalfd（幂等），fd_ 置 -1
void SignalWatcher::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

#else  // macOS / BSD：self-pipe（自管道）实现

// 信号 handler 写入的管道写端（进程级全局）。本实现约定单实例：
// 一个进程只有一个 SignalWatcher（demo_server 用 MultiServer，仅一个），
// 多实例同时收信号不被支持（后 init 者接管 handler 与写端）。
static int g_sig_write_fd = -1;

// 信号处理函数：把信号号写进管道，唤醒等待中的 wait()。
// 必须 async-signal-safe：只调用 write()，不触碰任何非安全函数。
static void SigPipeHandler(int sig) {
    int fd = g_sig_write_fd;
    if (fd >= 0) {
        unsigned char b = (unsigned char)sig;
        ssize_t r = ::write(fd, &b, 1);
        (void)r;  // 管道满时丢弃：信号本就允许 coalesce（合并）
    }
}

// 初始化 self-pipe：创建管道 + 为各信号安装 handler。
// macOS/BSD 没有 signalfd，若像 Linux 一样先 block 信号，handler 永远不会
// 执行，SIGTERM/SIGINT 会一直停在 pending 状态。这里由 handler 写管道唤醒
// 协程，因此必须保持信号未屏蔽。
// 参数：sigs - 需监听的信号集合。成功返回 true（fd_ 为管道读端）
bool SignalWatcher::init(const std::vector<int>& sigs) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        int fl = ::fcntl(fds[i], F_GETFL, 0);
        ::fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }

    struct sigaction sa{};
    sa.sa_handler = SigPipeHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (int s : sigs) ::sigaction(s, &sa, nullptr);

    g_sig_write_fd = fds[1];
    fd_ = fds[0];  // 读端：可读即代表有信号到达
    return true;
}

// 协程等待下一个信号：读端就绪后读 1 字节信号号返回
// 参数：无。返回信号编号；失败/非预期返回 -1
coro::Task<int> SignalWatcher::wait() {
    for (;;) {
        auto r = co_await coro::await_readable(fd_);
        (void)r;
        unsigned char b = 0;
        ssize_t n = ::read(fd_, &b, 1);
        if (n == 1) co_return (int)b;
        if (n < 0 && errno == EAGAIN) continue;
        co_return -1;
    }
}

// 关闭管道（幂等），fd_ 与全局写端置 -1
void SignalWatcher::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (g_sig_write_fd >= 0) { ::close(g_sig_write_fd); g_sig_write_fd = -1; }
}

#endif  // __linux__

}  // namespace net
