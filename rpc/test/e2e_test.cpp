// rpc 端到端测试：registry + greeter server + client 三进程（本测试用三个线程模拟）。
//
//   registry 线程：RpcServer + RegistryService（注册中心，租约 TTL）
//   greeter 线程：RpcServer + GreeterImpl + 注册到 registry + 周期心跳 + 退出注销
//   main 线程：  client 协程 Discover → 直连 → SayHello + Chat 断言 → 触发注销
//               → 再 Discover 断言列表变空
//
// 三个 EventLoop 各属一线程，互不共享协程；协程一律命名函数。
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "coro/awaiter.h"
#include "coro/event_loop.h"
#include "coro/task.h"
#include "rpc/error.h"
#include "rpc/init.h"
#include "rpc/rpc_channel.h"
#include "rpc/rpc_server.h"
#include "rpc/registry/registry_service.h"
#include "registry.pb.h"
#include "registry.rpc.h"
#include "greeter.pb.h"
#include "greeter.rpc.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);        \
        }                                                                      \
    } while (0)

// ══════════════════ 跨线程信号 ══════════════════
static std::atomic<int> g_registry_port{0};  // registry server 实际端口（线程就绪信号）
static std::atomic<int> g_greeter_port{0};   // greeter server 实际端口（线程就绪信号）
static std::atomic<bool> g_deregister_now{false};  // 主线程 → greeter：请注销
static std::atomic<bool> g_deregistered{false};    // greeter → 主线程：已注销
static std::atomic<bool> g_registry_stop{false};   // 主线程 → registry：请停止

// ══════════════════ greeter 业务实现 ══════════════════
class GreeterImpl : public greeter::GreeterServiceBase {
public:
    coro::Task<::greeter::HelloReply> SayHello(const ::greeter::HelloRequest& req) override {
        ::greeter::HelloReply reply;
        reply.set_message("Hello, " + req.name() + "!");
        co_return reply;
    }
    coro::Task<void> Chat(rpc::ServerReaderWriter<::greeter::HelloRequest,
                                                  ::greeter::HelloReply>& stream) override {
        for (;;) {
            ::greeter::HelloRequest req;
            if (!co_await stream.Read(&req)) break;
            ::greeter::HelloReply reply;
            reply.set_message("Hi, " + req.name() + "!");
            co_await stream.Write(reply);
        }
        co_await stream.Finish();
    }
};

// ══════════════════ registry 线程协程 ══════════════════
// 周期清理：每秒剔除租约过期实例；同进程测试租约短，方便验证 TTL
static coro::Task<void> e2e_registry_sweep(rpc::RegistryService& reg) {
    while (!g_registry_stop.load()) {
        reg.SweepExpired();
        co_await coro::sleep_for(500);
    }
    co_return;
}
// 停止 watcher：主线程置 g_registry_stop 后退出 loop
static coro::Task<void> e2e_registry_watcher() {
    while (!g_registry_stop.load()) co_await coro::sleep_for(20);
    coro::EventLoop::current().stop();
    co_return;
}

// registry 线程入口：独立 EventLoop 承载注册中心
static void registry_thread_main() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    if (!server.Start("127.0.0.1", 0)) {  // 端口 0：内核分配，经原子变量广播
        std::printf("FAIL: registry server 启动失败\n");
        return;
    }
    g_registry_port.store(server.port());
    rpc::RegistryService registry;
    rpc::registry::RegisterRegistryService(server, registry);

    coro::Task<void> serve = server.Serve();
    coro::Task<void> sweep = e2e_registry_sweep(registry);
    coro::Task<void> watcher = e2e_registry_watcher();
    loop.post(serve.handle());
    loop.post(sweep.handle());
    loop.post(watcher.handle());
    loop.run();
}

// ══════════════════ greeter 线程协程 ══════════════════
// 注册 + 心跳 + 注销：注册拿 instance_id，周期心跳续租约；
// 主线程请求注销后 Deregister 并停止本线程 loop
static coro::Task<void> e2e_greeter_register(int rport, int gport) {
    constexpr std::int64_t kLeaseSeconds = 10;
    constexpr std::int64_t kHeartbeatMs = 1000;

    auto rch = std::make_shared<rpc::RpcChannel>();
    if (!co_await rch->Open("127.0.0.1", rport, 3000)) {
        std::printf("FAIL: greeter 连不上 registry\n");
        coro::EventLoop::current().stop();
        co_return;
    }
    rpc::registry::RegistryClient reg(*rch);
    rpc::registry::RegisterRequest rr;
    rpc::registry::Instance* inst = rr.mutable_instance();
    inst->set_service_name("greeter.Greeter");
    inst->set_host("127.0.0.1");
    inst->set_port(static_cast<std::uint32_t>(gport));
    inst->set_lease_seconds(kLeaseSeconds);
    rpc::registry::RegisterReply rep = co_await reg.Register(rr, 3000);
    if (rep.instance_id().empty()) {
        std::printf("FAIL: greeter 注册失败\n");
        coro::EventLoop::current().stop();
        co_return;
    }
    const std::string id = rep.instance_id();

    while (!g_deregister_now.load()) {  // 心跳循环：直到主线程请求注销
        co_await coro::sleep_for(kHeartbeatMs);
        try {
            rpc::registry::HeartbeatRequest hq;
            hq.set_instance_id(id);
            (void)co_await reg.Heartbeat(hq, 3000);
        } catch (const rpc::RpcException&) {
            break;  // registry 不可达：无法续期，停止心跳
        }
    }
    try {
        rpc::registry::DeregisterRequest dq;
        dq.set_instance_id(id);
        (void)co_await reg.Deregister(dq, 3000);
    } catch (const rpc::RpcException&) {
        // registry 不可达：注销失败，主线程复查时按注册中心状态判断
    }
    g_deregistered.store(true);  // 通知主线程：可复查列表为空

    rch->Close();
    while (!rch->read_done()) co_await coro::sleep_for(10);
    coro::EventLoop::current().stop();
    co_return;
}

// greeter 线程入口：独立 EventLoop 承载业务服务
static void greeter_thread_main(int rport) {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    if (!server.Start("127.0.0.1", 0)) {
        std::printf("FAIL: greeter server 启动失败\n");
        return;
    }
    g_greeter_port.store(server.port());
    GreeterImpl impl;
    greeter::RegisterGreeterService(server, impl);

    coro::Task<void> serve = server.Serve();
    coro::Task<void> reg = e2e_greeter_register(rport, server.port());
    loop.post(serve.handle());
    loop.post(reg.handle());
    loop.run();
}

// ══════════════════ 主线程：客户端端到端 ══════════════════
static coro::Task<void> e2e_client(int rport) {
    // 1) 服务发现：应恰好 1 个 greeter 实例
    auto rch = std::make_shared<rpc::RpcChannel>();
    if (!co_await rch->Open("127.0.0.1", rport, 3000)) {
        CHECK(false, "客户端连不上 registry");
        coro::EventLoop::current().stop();
        co_return;
    }
    rpc::registry::RegistryClient reg(*rch);
    rpc::registry::DiscoverRequest dq;
    dq.set_service_name("greeter.Greeter");
    rpc::registry::DiscoverReply drep = co_await reg.Discover(dq, 3000);
    CHECK(drep.instances_size() == 1, "Discover 找到 1 个 greeter 实例");
    const rpc::registry::Instance& inst = drep.instances(0);
    CHECK(inst.service_name() == "greeter.Greeter", "实例 service_name 正确");
    CHECK(inst.host() == "127.0.0.1", "实例 host 正确");
    CHECK(inst.port() == static_cast<std::uint32_t>(g_greeter_port.load()), "实例 port 正确");

    // 2) 直连实例：一元 + 双向流
    auto ch = std::make_shared<rpc::RpcChannel>();
    CHECK(co_await ch->Open(inst.host(), inst.port(), 3000), "客户端连上 greeter 实例");
    greeter::GreeterClient client(*ch);

    greeter::HelloRequest hq;
    hq.set_name("World");
    greeter::HelloReply hr = co_await client.SayHello(hq, 3000);
    CHECK(hr.message() == "Hello, World!", "一元 SayHello 往返正确");

    auto st = co_await client.Chat(3000);
    for (const char* n : {"Alice", "Bob", "Carol"}) {
        greeter::HelloRequest r;
        r.set_name(n);
        co_await st->Write(r);
    }
    co_await st->CloseWrite();
    std::vector<std::string> chat;
    for (;;) {
        rpc::StreamEvent ev = co_await st->Read(3000);
        if (ev.kind != rpc::StreamEventKind::Data) break;
        greeter::HelloReply m;
        if (m.ParseFromString(ev.payload)) chat.push_back(m.message());
    }
    CHECK(chat.size() == 3, "双向流收到 3 条回复");
    bool chat_ok = chat.size() == 3 && chat[0] == "Hi, Alice!" &&
                   chat[1] == "Hi, Bob!" && chat[2] == "Hi, Carol!";
    CHECK(chat_ok, "双向流内容逐条正确");

    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);

    // 3) 触发 greeter 注销，等它完成
    g_deregister_now.store(true);
    while (!g_deregistered.load()) co_await coro::sleep_for(10);

    // 4) 复查：Discover 应变空（注销生效）
    drep = co_await reg.Discover(dq, 3000);
    CHECK(drep.instances_size() == 0, "Deregister 后 Discover 列表为空");

    rch->Close();
    while (!rch->read_done()) co_await coro::sleep_for(10);
    coro::EventLoop::current().stop();
    co_return;
}

int main() {
    rpc::Init();

    std::thread rt(registry_thread_main);
    while (g_registry_port.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::thread gt(greeter_thread_main, g_registry_port.load());
    while (g_greeter_port.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    coro::EventLoop loop;
    coro::Task<void> c = e2e_client(g_registry_port.load());
    loop.post(c.handle());
    loop.run();

    // 收尾：停 registry（greeter 线程已自行停止）
    g_registry_stop.store(true);
    rt.join();
    gt.join();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
