// greeter 客户端示例：用 RpcClient 门面（发现 + 连接池 + 重试）调用服务。
//   一元 SayHello：走 CallUnary（自动发现实例、失败重试、实例间 fallback）
//   双向流 Chat：  走 Acquire 借连接，用生成代码桩开启流
// 用法：greeter_client [registry_host] [registry_port]   默认 127.0.0.1:56788
#include <cstdio>
#include <cstdlib>
#include <string>

#include "coro/awaiter.h"
#include "coro/event_loop.h"
#include "coro/task.h"
#include "rpc/error.h"
#include "rpc/init.h"
#include "rpc/rpc_client.h"
#include "greeter.pb.h"
#include "greeter.rpc.h"

// 客户端协程：RpcClient 门面 → 一元 + 双向流 → CloseAll → 停止事件循环
static coro::Task<void> run_client(const std::string& rhost, unsigned rport) {
    rpc::RpcClient client(rhost, rport);  // 门面：连接池 + 发现缓存 + 重试
    try {
        // 一元：CallUnary 自动发现实例并调用（失败自动重试换实例）
        greeter::HelloRequest req;
        req.set_name("World");
        std::string bytes = co_await client.CallUnary(
            "greeter.Greeter", "SayHello", req.SerializeAsString(), 3000);
        greeter::HelloReply rep;
        if (rep.ParseFromString(bytes)) std::printf("SayHello => %s\n", rep.message().c_str());

        // 双向流：Acquire 借连接 → 生成代码桩开流 → 用毕归还
        auto ch = co_await client.Acquire("greeter.Greeter", 3000);
        if (!ch) throw rpc::RpcException(rpc::RpcCode::NotFound, "无可用实例");
        greeter::GreeterClient gc(*ch);
        auto st = co_await gc.Chat(3000);
        for (const char* n : {"Alice", "Bob", "Carol"}) {
            greeter::HelloRequest r;
            r.set_name(n);
            co_await st->Write(r);
        }
        co_await st->CloseWrite();  // 半关闭输入：服务端读到结束会回完并关闭
        for (;;) {
            rpc::StreamEvent ev = co_await st->Read(3000);
            if (ev.kind != rpc::StreamEventKind::Data) break;  // Done / Timeout 均结束
            greeter::HelloReply hr;
            if (hr.ParseFromString(ev.payload)) std::printf("Chat <= %s\n", hr.message().c_str());
        }
        client.Release(ch);
    } catch (const rpc::RpcException& e) {
        std::printf("RPC 错误：%s\n", e.what());
    }
    co_await client.CloseAll();  // 关闭全部连接池与 registry 连接
    coro::EventLoop::current().stop();
    co_return;
}

int main(int argc, char** argv) {
    rpc::Init();
    const char* rhost = argc > 1 ? argv[1] : "127.0.0.1";
    const unsigned rport = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 56788;

    coro::EventLoop loop;
    coro::Task<void> c = run_client(rhost, rport);
    loop.post(c.handle());
    loop.run();
    return 0;
}
