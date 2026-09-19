// registry 注册中心示例：独立进程承载服务发现。
// 业务服务端 Register/Heartbeat 上报实例；客户端 Discover 获取实例列表直连。
// 用法：registry_server [host] [port]    默认 127.0.0.1:56788
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "coro/awaiter.h"
#include "coro/event_loop.h"
#include "coro/task.h"
#include "rpc/init.h"
#include "rpc/rpc_server.h"
#include "rpc/registry/registry_service.h"
#include "registry.pb.h"
#include "registry.rpc.h"

// 退出标志：信号处理器只置位，轮询协程检查
static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// 轮询退出标志：SIGINT/SIGTERM 后停止事件循环
static coro::Task<void> signal_watcher() {
    while (!g_stop) co_await coro::sleep_for(50);
    coro::EventLoop::current().stop();
    co_return;
}

// TTL 清理协程：每秒剔除一次租约过期实例（registry 默认租约 60s）
static coro::Task<void> sweep_loop(rpc::RegistryService& reg) {
    while (!g_stop) {
        std::size_t n = reg.SweepExpired();
        if (n > 0) std::printf("registry: 剔除 %zu 个过期实例\n", n);
        co_await coro::sleep_for(1000);
    }
    co_return;
}

int main(int argc, char** argv) {
    rpc::Init();  // 屏蔽 SIGPIPE
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const unsigned port = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 56788;

    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    if (!server.Start(host, port)) {
        std::fprintf(stderr, "监听 %s:%u 失败\n", host, port);
        return 1;
    }
    rpc::RegistryService registry;                       // 内存存储：生命周期覆盖整个 loop
    rpc::registry::RegisterRegistryService(server, registry);  // 生成代码：注册 4 个方法

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("registry_server 监听 %s:%u（Ctrl+C 退出）\n", host, server.port());
    std::fflush(stdout);

    coro::Task<void> serve = server.Serve();
    coro::Task<void> watcher = signal_watcher();
    coro::Task<void> sweep = sweep_loop(registry);
    loop.post(serve.handle());
    loop.post(watcher.handle());
    loop.post(sweep.handle());
    loop.run();
    return 0;
}
