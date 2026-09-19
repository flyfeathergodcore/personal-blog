// greeter 服务端示例：继承 protoc 插件生成的 GreeterServiceBase 实现业务，
// 注册进 RpcServer，并向 registry 注册实例 + 周期心跳（租约 10s，每 3s 续期）。
// 退出时向 registry 注销，再停止事件循环。
// 用法：greeter_server [host] [port] [registry_host] [registry_port]
//       默认 host=127.0.0.1 port=56789 registry=127.0.0.1:56788
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <memory>
#include <string>

#include "coro/awaiter.h"
#include "coro/event_loop.h"
#include "coro/task.h"
#include "rpc/init.h"
#include "rpc/rpc_channel.h"
#include "rpc/rpc_server.h"
#include "registry.pb.h"
#include "registry.rpc.h"
#include "greeter.pb.h"
#include "greeter.rpc.h"

// 业务实现：继承生成的服务基类（greeter 命名空间），实现各 rpc 虚函数
class GreeterImpl : public greeter::GreeterServiceBase {
public:
    // 一元：SayHello 返回问候语
    coro::Task<::greeter::HelloReply> SayHello(const ::greeter::HelloRequest& request) override {
        ::greeter::HelloReply reply;
        reply.set_message("Hello, " + request.name() + "!");
        co_return reply;
    }

    // 双向流：Chat 逐条读入名字并流式回问候语；客户端半关闭后结束服务端输出
    coro::Task<void> Chat(rpc::ServerReaderWriter<::greeter::HelloRequest,
                                                  ::greeter::HelloReply>& stream) override {
        for (;;) {
            ::greeter::HelloRequest req;
            if (!co_await stream.Read(&req)) break;  // 客户端半关闭 → 输入结束
            ::greeter::HelloReply reply;
            reply.set_message("Hi, " + req.name() + "!");
            co_await stream.Write(reply);
        }
        co_await stream.Finish();  // 结束服务端输出（半关闭写出端）
    }
};

// 退出标志：信号处理器只置位（异步信号安全），由各协程轮询检查
static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// 心跳协程：每 interval_ms 向 registry 续期一次，实例被剔除则告警；
// registry 不可达时捕获异常并告警（不中断循环，等恢复或退出信号）
static coro::Task<void> heartbeat_loop(std::shared_ptr<rpc::RpcChannel> rch,
                                       const std::string& instance_id, int64_t interval_ms) {
    rpc::registry::RegistryClient reg(*rch);  // 生成代码：registry 客户端桩
    while (!g_stop) {
        co_await coro::sleep_for(interval_ms);
        try {
            rpc::registry::HeartbeatRequest req;
            req.set_instance_id(instance_id);
            rpc::registry::HeartbeatReply rep = co_await reg.Heartbeat(req, 3000);
            if (!rep.ok()) std::printf("registry 心跳失败（实例可能已被剔除）\n");
        } catch (const rpc::RpcException& e) {
            std::printf("registry 心跳不可达：%s\n", e.what());
        }
    }
    co_return;
}

// 注册协程：连 registry → Register 拿 instance_id → 起心跳 → 等退出信号 → Deregister → 停循环
static coro::Task<void> register_to_registry(const std::string& rhost, unsigned rport,
                                             const std::string& shost, unsigned sport) {
    constexpr std::int64_t kLeaseSeconds = 10;   // 租约：10s
    constexpr std::int64_t kHeartbeatMs = 3000;  // 心跳：每 3s 一次（租约 1/3）
    const std::string svc = "greeter.Greeter";

    auto rch = std::make_shared<rpc::RpcChannel>();
    if (!co_await rch->Open(rhost, rport, 3000)) {
        std::printf("连接 registry %s:%u 失败，仍以直连模式运行\n", rhost.c_str(), rport);
        co_return;
    }
    rpc::registry::RegistryClient reg(*rch);

    rpc::registry::RegisterRequest req;
    rpc::registry::Instance* inst = req.mutable_instance();
    inst->set_service_name(svc);
    inst->set_host(shost);
    inst->set_port(sport);
    inst->set_lease_seconds(kLeaseSeconds);
    rpc::registry::RegisterReply rep = co_await reg.Register(req, 3000);
    if (rep.instance_id().empty()) {
        std::printf("registry 注册失败（参数不合法）\n");
        co_return;
    }
    const std::string id = rep.instance_id();
    std::printf("已注册到 registry：%s（%s:%u，租约 %llds）\n", id.c_str(),
                shost.c_str(), sport, static_cast<long long>(kLeaseSeconds));
    std::fflush(stdout);

    // 心跳协程持 rch 的一份 shared_ptr，与注册协程共享连接
    coro::EventLoop& loop = coro::EventLoop::current();
    coro::Task<void> hb = heartbeat_loop(rch, id, kHeartbeatMs);
    loop.post(hb.handle());

    while (!g_stop) co_await coro::sleep_for(100);  // 等退出信号

    // 优雅注销：registry 已先退出时 UnaryCall 会抛异常，捕获后同样保证退出
    try {
        rpc::registry::DeregisterRequest dr;
        dr.set_instance_id(id);
        (void)co_await reg.Deregister(dr, 3000);
        std::printf("已从 registry 注销\n");
    } catch (const rpc::RpcException& e) {
        std::printf("注销时 registry 不可达：%s\n", e.what());
    }
    rch->Close();
    while (!rch->read_done()) co_await coro::sleep_for(10);  // 等读循环退出再析构
    loop.stop();  // 唯一 stop 点：心跳/Serve 协程随之停止，进程退出
    co_return;
}

int main(int argc, char** argv) {
    rpc::Init();  // 屏蔽 SIGPIPE：对端断开后晚到写不崩进程
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const unsigned port = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 56789;
    const char* rhost = argc > 3 ? argv[3] : "127.0.0.1";
    const unsigned rport = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 56788;

    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    if (!server.Start(host, port)) {
        std::fprintf(stderr, "监听 %s:%u 失败\n", host, port);
        return 1;
    }
    GreeterImpl impl;
    greeter::RegisterGreeterService(server, impl);  // 生成代码：注册全部方法

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("greeter_server 监听 %s:%u（Ctrl+C 退出）\n", host, server.port());
    std::fflush(stdout);

    coro::Task<void> serve = server.Serve();
    coro::Task<void> reg = register_to_registry(rhost, rport, host, server.port());
    loop.post(serve.handle());
    loop.post(reg.handle());
    loop.run();
    return 0;
}
