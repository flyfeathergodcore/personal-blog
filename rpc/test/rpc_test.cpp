// rpc 框架单元测试：帧编解码 / 写并发 / 多路复用 / 超时 / 双向流 / 连接池。
// 断言风格沿用 webcpp-engine/test/net_test.cpp：CHECK 宏 + 原子计数 + 独立 EventLoop。
// 注意：协程一律用命名函数（严禁 [&] 捕获的协程 lambda——悬垂闭包陷阱）。
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coro/awaiter.h"
#include "coro/event_loop.h"
#include "coro/task.h"
#include "rpc/error.h"
#include "rpc/frame.h"
#include "rpc/init.h"
#include "rpc/rpc_channel.h"
#include "rpc/rpc_client.h"
#include "rpc/rpc_pool.h"
#include "rpc/rpc_server.h"
#include "rpc/rpc_stream.h"
#include "rpc/write_lock.h"
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

// ══════════════════ 1. 帧编解码（同步，无需 EventLoop）══════════════════

// 测试 Encode 长度前缀：4 字节大端 + protobuf 负载
static void test_frame_encode_roundtrip() {
    rpc::RpcFrame f;
    f.set_kind(rpc::RpcKind::UNARY_REQUEST);
    f.set_call_id(7);
    f.set_service_name("greeter.Greeter");
    f.set_method_name("SayHello");
    f.set_payload("hello world");

    std::string bytes;
    CHECK(rpc::FrameCodec::Encode(f, &bytes), "Encode 成功");
    CHECK(bytes.size() >= 4, "Encode 输出含 4 字节前缀");

    // 长度前缀 = payload 序列化长度；payload 应能完整解回
    uint32_t len = 0;
    const uint8_t* hdr = reinterpret_cast<const uint8_t*>(bytes.data());
    CHECK(rpc::FrameCodec::TryDecodeHeader(hdr, &len), "TryDecodeHeader 成功");
    CHECK(len + 4 == bytes.size(), "长度前缀与总长一致");

    // 反序列化还原
    rpc::RpcFrame out;
    CHECK(out.ParseFromString(bytes.substr(4)), "payload 反序列化成功");
    CHECK(out.kind() == rpc::RpcKind::UNARY_REQUEST, "kind 还原");
    CHECK(out.call_id() == 7, "call_id 还原");
    CHECK(out.service_name() == "greeter.Greeter", "service_name 还原");
    CHECK(out.method_name() == "SayHello", "method_name 还原");
    CHECK(out.payload() == "hello world", "payload 还原");
}

// 测试大端字节序：手工构造长度头验证 TryDecodeHeader 解析
static void test_frame_len_prefix_bigendian() {
    // len=5 → 字节 [0,0,0,5]
    uint8_t h5[4] = {0, 0, 0, 5};
    uint32_t len = 0;
    CHECK(rpc::FrameCodec::TryDecodeHeader(h5, &len), "小长度头解析成功");
    CHECK(len == 5, "大端解析 len=5");

    // len=256 → 字节 [0,0,1,0]（验证多字节位权）
    uint8_t h256[4] = {0, 0, 1, 0};
    CHECK(rpc::FrameCodec::TryDecodeHeader(h256, &len), "256 长度头解析成功");
    CHECK(len == 256, "大端解析 len=256");
}

// 测试空 payload：长度 0 也合法
static void test_frame_encode_empty() {
    std::string bytes;
    rpc::RpcFrame f;
    CHECK(rpc::FrameCodec::Encode(f, &bytes), "空帧 Encode 成功");
    uint32_t len = 0;
    CHECK(rpc::FrameCodec::TryDecodeHeader(
              reinterpret_cast<const uint8_t*>(bytes.data()), &len),
          "空帧头解析成功");
    CHECK(len == 0, "空帧长度为 0");
}

// 测试超长长度拒绝：长度头 > kMaxFrameLen 必须被 TryDecodeHeader 拒绝
static void test_frame_oversize_reject() {
    // 手工构造超长长度头（大端）
    uint32_t bad = rpc::kMaxFrameLen + 1;
    uint8_t hdr[4] = {
        static_cast<uint8_t>((bad >> 24) & 0xff),
        static_cast<uint8_t>((bad >> 16) & 0xff),
        static_cast<uint8_t>((bad >> 8) & 0xff),
        static_cast<uint8_t>(bad & 0xff),
    };
    uint32_t len = 0;
    CHECK(!rpc::FrameCodec::TryDecodeHeader(hdr, &len), "超长长度头被拒绝");
}

// ════════════ 2. WriteLock 写并发（多协程串行写帧，字节不交错）════════════

// 写端协程：经 WriteLock 串行写出 frames 帧；每帧 payload 唯一标识 w<id>-f<idx>
// 完成时 remaining 递增；最后一个协程完成时 stop 事件循环（run() 以 stop 为退出条件）
static coro::Task<void> lock_writer(std::shared_ptr<net::TcpStream> s,
                                    std::shared_ptr<rpc::WriteLock> lock,
                                    int id, int frames, std::atomic<int>* done,
                                    std::atomic<int>* remaining, int total_coros) {
    for (int i = 0; i < frames; ++i) {
        rpc::RpcFrame f;
        f.set_kind(rpc::RpcKind::UNARY_REQUEST);
        f.set_call_id(static_cast<uint64_t>(id * 1000 + i));
        f.set_payload("w" + std::to_string(id) + "-f" + std::to_string(i));
        // 无限等待：空闲直接持有，否则 FIFO 排队；拿到锁才写
        if (!co_await lock->Acquire(-1)) {
            ++*done;
            co_return;
        }
        co_await rpc::WriteFrame(*s, f);
        lock->Release();
    }
    ++*done;
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

// 读端协程：逐帧 ReadFrame，收满 total 帧即返回；payload 收集进 seen
static coro::Task<void> lock_reader(std::shared_ptr<net::TcpStream> s, int total,
                                    std::atomic<int>* frames_ok,
                                    std::vector<std::string>* seen,
                                    std::atomic<int>* remaining, int total_coros) {
    int got = 0;
    for (;;) {
        rpc::RpcFrame f;
        net::IoResult r = co_await rpc::ReadFrame(*s, &f, 5000);
        if (!r.ok()) break;  // EOF / 错误
        if (f.kind() == rpc::RpcKind::UNARY_REQUEST) {
            seen->push_back(f.payload());
            if (++got >= total) break;
        }
    }
    *frames_ok = got;
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

// 16 写协程 × 8 帧经同一 WriteLock 并发写，读端按帧解析全部收齐且 payload 唯一
//（任何字节交错都会破坏长度前缀 → ReadFrame 失败或 payload 错乱，必现）
// 读端要等全部帧写过来才收满，因此它总是最后完成并 stop 事件循环
static void test_write_lock_concurrent() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "写并发 socketpair 创建");
    for (int i = 0; i < 2; ++i) {
        int fl = fcntl(sv[i], F_GETFL, 0);
        fcntl(sv[i], F_SETFL, fl | O_NONBLOCK);
    }
    // 两端各包成 TcpStream（接管 fd，析构关闭）；写端共享同一锁
    auto ws = std::make_shared<net::TcpStream>(sv[0]);
    auto rs = std::make_shared<net::TcpStream>(sv[1]);
    auto lock = std::make_shared<rpc::WriteLock>();

    constexpr int kWriters = 16, kFrames = 8, kTotal = kWriters * kFrames;
    constexpr int kCoros = kWriters + 1;
    std::atomic<int> done{0}, frames_ok{0}, remaining{0};
    std::vector<std::string> seen;
    seen.reserve(kTotal);

    coro::EventLoop loop;
    for (int i = 0; i < kWriters; ++i) {
        coro::Task<void> t = lock_writer(ws, lock, i, kFrames, &done, &remaining, kCoros);
        loop.post(t.handle());
    }
    coro::Task<void> r = lock_reader(rs, kTotal, &frames_ok, &seen, &remaining, kCoros);
    loop.post(r.handle());
    loop.run();

    CHECK(frames_ok == kTotal, "读端收满全部帧");
    CHECK(done == kWriters, "全部写协程完成");
    // payload 全部唯一 = 无交错；排序后找相邻重复
    bool unique = seen.size() == static_cast<size_t>(kTotal);
    if (unique) {
        std::vector<std::string> sorted = seen;
        std::sort(sorted.begin(), sorted.end());
        unique = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    }
    CHECK(unique, "全部帧 payload 唯一（字节无交错）");
}

// ════════════ 3. WriteLock 超时（占锁期间 Acquire 超时返回 false）════════════

// 占锁协程：拿锁后睡 200ms 再释放；stage 1=拿锁 2=释放（stage 只由 holder 写）
// 完成时 remaining 递增；最后一个完成者 stop 事件循环
static coro::Task<void> lock_holder(std::shared_ptr<rpc::WriteLock> lock,
                                    std::atomic<int>* stage,
                                    std::atomic<int>* remaining, int total_coros) {
    bool ok = co_await lock->Acquire(-1);
    CHECK(ok, "holder 拿到锁");
    *stage = 1;
    co_await coro::sleep_for(200);
    lock->Release();
    *stage = 2;
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

// 超时协程：等 holder 拿锁后 Acquire(80ms) 应超时返回 false（不写 stage，避免污染 holder 阶段）
static coro::Task<void> lock_waiter(std::shared_ptr<rpc::WriteLock> lock,
                                    std::atomic<int>* stage, int* result,
                                    std::atomic<int>* remaining, int total_coros) {
    while (*stage < 1) co_await coro::sleep_for(10);  // 等 holder 拿锁
    bool ok = co_await lock->Acquire(80);
    *result = ok ? 1 : 0;
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

// 迟到协程：等 holder 释放（stage==2）后再 Acquire 应成功（验证超时移除不破坏等待队列）
static coro::Task<void> lock_late(std::shared_ptr<rpc::WriteLock> lock,
                                  std::atomic<int>* stage, int* result,
                                  std::atomic<int>* remaining, int total_coros) {
    while (*stage != 2) co_await coro::sleep_for(10);  // 等 holder 释放
    bool ok = co_await lock->Acquire(1000);
    *result = ok ? 1 : 0;
    lock->Release();
    *stage = 4;
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_write_lock_timeout() {
    auto lock = std::make_shared<rpc::WriteLock>();
    std::atomic<int> stage{0}, remaining{0};
    int waiter_result = -1, late_result = -1;
    constexpr int kCoros = 3;

    coro::EventLoop loop;
    coro::Task<void> h = lock_holder(lock, &stage, &remaining, kCoros);
    coro::Task<void> w = lock_waiter(lock, &stage, &waiter_result, &remaining, kCoros);
    coro::Task<void> l = lock_late(lock, &stage, &late_result, &remaining, kCoros);
    loop.post(h.handle());
    loop.post(w.handle());
    loop.post(l.handle());
    loop.run();

    CHECK(stage == 4, "holder/waiter/late 三阶段全部走完");
    CHECK(waiter_result == 0, "占锁期间 Acquire 超时返回 false");
    CHECK(late_result == 1, "释放后 Acquire 成功");
}

// ════════════ 4. 一元调用全链路（服务端 + 客户端 loopback）════════════

// 客户端协程：Open → UnaryCall → 校验响应 → Close → stop
static coro::Task<void> rpc_echo_client(int port, std::string* out,
                                        std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    bool ok = co_await ch->Open("127.0.0.1", port, 3000);
    if (!ok) {
        *out = "CONN_FAIL";
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    try {
        *out = co_await ch->UnaryCall("test.Echo", "SayHello", "world", 3000);
    } catch (const rpc::RpcException& e) {
        *out = "ERR:" + std::string(e.what());
    }
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);  // 等读循环退出再析构
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_unary_roundtrip() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "一元 server 监听成功");
    CHECK(server.port() > 0, "一元 server 拿到实际端口");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Unary;
    mh.unary = [](std::string req, const rpc::ServerCallContext&) -> coro::Task<std::string> {
        co_return "echo:" + req;  // 空捕获协程 lambda：无闭包悬垂风险
    };
    server.Register("test.Echo", "SayHello", std::move(mh));

    std::string out;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = rpc_echo_client(server.port(), &out, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(out == "echo:world", "一元调用往返返回 echo:world");
}

// ════════════ 5. 32 路并发多路复用（同 channel 按 call_id 分派）════════════

// 单个并发调用协程：完成即 done++（不参与 stop 计数，由 mux_client 统一等待）
static coro::Task<void> rpc_mux_call(rpc::RpcChannel* ch, int i,
                                     std::vector<std::string>* out,
                                     std::atomic<int>* done) {
    try {
        std::string resp = co_await ch->UnaryCall("test.Echo", "SayHello",
                                                  "m" + std::to_string(i), 3000);
        (*out)[i] = resp;
    } catch (const rpc::RpcException& e) {
        (*out)[i] = "ERR";
    }
    ++*done;
    co_return;
}

// 客户端协程：并发 spawn 全部调用（同 channel 多路复用），等齐 → Close → stop
static coro::Task<void> rpc_mux_client(int port, int n, std::vector<std::string>* out,
                                       std::atomic<int>* done,
                                       std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    bool ok = co_await ch->Open("127.0.0.1", port, 3000);
    if (!ok) {
        CHECK(false, "多路复用 connect 失败");
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    for (int i = 0; i < n; ++i) {
        coro::Task<void> t = rpc_mux_call(ch.get(), i, out, done);
        coro::EventLoop::current().post(t.handle());
    }
    while (*done < n) co_await coro::sleep_for(10);  // 等全部调用完成
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);  // 等读循环退出再析构
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_channel_mux() {
    constexpr int kCalls = 32;
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "多路复用 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Unary;
    mh.unary = [](std::string req, const rpc::ServerCallContext&) -> coro::Task<std::string> {
        co_return "echo:" + req;
    };
    server.Register("test.Echo", "SayHello", std::move(mh));

    std::vector<std::string> out(kCalls);
    std::atomic<int> done{0}, remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = rpc_mux_client(server.port(), kCalls, &out, &done, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(done == kCalls, "全部 32 路调用完成");
    bool all_ok = true;
    for (int i = 0; i < kCalls; ++i) {
        if (out[i] != "echo:m" + std::to_string(i)) { all_ok = false; break; }
    }
    CHECK(all_ok, "32 路响应按 call_id 正确分发（无串扰）");
}

// ════════════ 6. 一元调用超时（慢 handler，50ms 超时）════════════

// 客户端协程：调用 300ms 慢 handler，期望 50ms 超时抛 RpcException(Timeout)
static coro::Task<void> rpc_timeout_client(int port, std::string* out,
                                           std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    bool ok = co_await ch->Open("127.0.0.1", port, 3000);
    if (!ok) {
        *out = "CONN_FAIL";
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    try {
        co_await ch->UnaryCall("test.Slow", "Delay", "x", 50);
        *out = "NO_TIMEOUT";
    } catch (const rpc::RpcException& e) {
        *out = (e.code() == rpc::RpcCode::Timeout) ? "TIMEOUT" : "WRONG_ERR";
    }
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_unary_timeout() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "超时 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Unary;
    mh.unary = [](std::string, const rpc::ServerCallContext&) -> coro::Task<std::string> {
        co_await coro::sleep_for(300);  // 慢 handler：300ms 才返回
        co_return "slow";
    };
    server.Register("test.Slow", "Delay", std::move(mh));

    std::string out;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = rpc_timeout_client(server.port(), &out, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(out == "TIMEOUT", "50ms 超时返回 Timeout（慢 handler 300ms）");
}

// ════════════ 7. 服务端流（客户端流式请求后半关闭，服务端流式返回）════════════

// 客户端协程：开流 → 连写 3 条 → 半关闭 → 逐条读到 Done。
// 服务端 handler 不显式 Finish，验证框架补发 STREAM_DONE{OK}（否则 Read 会超时漏数据）
static coro::Task<void> rpc_server_stream_client(int port, std::vector<std::string>* out,
                                                 std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    if (!co_await ch->Open("127.0.0.1", port, 3000)) {
        out->push_back("CONN_FAIL");
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    try {
        auto st = co_await ch->OpenStream("test.Stream", "ServerStream", 3000);
        co_await st->Write("a");
        co_await st->Write("b");
        co_await st->Write("c");
        co_await st->CloseWrite();
        for (;;) {
            rpc::StreamEvent ev = co_await st->Read(2000);
            if (ev.kind != rpc::StreamEventKind::Data) break;  // Done（或意外超时）
            out->push_back(ev.payload);
        }
    } catch (const rpc::RpcException& e) {
        out->push_back("ERR:" + std::string(e.what()));
    }
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);  // 等读循环退出再析构
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_stream_server_streaming() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "服务端流 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Stream;
    mh.stream = [](const rpc::ServerCallContext& ctx) -> coro::Task<void> {
        std::vector<std::string> got;
        for (;;) {
            rpc::StreamEvent ev = co_await ctx.reader->Read(2000);
            if (ev.kind != rpc::StreamEventKind::Data) break;  // 客户端 DONE → 结束
            got.push_back(ev.payload);
        }
        // 不显式 Finish：由框架在 handler 结束后补发 STREAM_DONE{OK}
        for (size_t i = 0; i < got.size(); ++i) {
            co_await ctx.writer->Write("resp:" + std::to_string(i) + ":" + got[i]);
        }
    };
    server.Register("test.Stream", "ServerStream", std::move(mh));

    std::vector<std::string> out;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = rpc_server_stream_client(server.port(), &out, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(out.size() == 3, "服务端流收到 3 条响应");
    if (out.size() == 3) {
        CHECK(out[0] == "resp:0:a" && out[1] == "resp:1:b" && out[2] == "resp:2:c",
              "服务端流响应顺序与内容正确");
    }
}

// ════════════ 8. 客户端流（客户端流式请求，服务端读齐后回一条汇总）════════════

// 客户端协程：开流 → 连写 2 条 → 半关闭 → 读汇总 → 读到 Done。
// 服务端 handler 显式 Finish，验证客户端能收 DONE 收尾
static coro::Task<void> rpc_client_stream_client(int port, std::string* out,
                                                 std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    if (!co_await ch->Open("127.0.0.1", port, 3000)) {
        *out = "CONN_FAIL";
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    try {
        auto st = co_await ch->OpenStream("test.Stream", "ClientStream", 3000);
        co_await st->Write("ab");
        co_await st->Write("cd");
        co_await st->CloseWrite();
        for (;;) {
            rpc::StreamEvent ev = co_await st->Read(2000);
            if (ev.kind == rpc::StreamEventKind::Data) {
                *out = ev.payload;  // 服务端汇总响应
                continue;
            }
            break;  // Done
        }
    } catch (const rpc::RpcException& e) {
        *out = "ERR:" + std::string(e.what());
    }
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_stream_client_streaming() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "客户端流 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Stream;
    mh.stream = [](const rpc::ServerCallContext& ctx) -> coro::Task<void> {
        std::string all;
        for (;;) {
            rpc::StreamEvent ev = co_await ctx.reader->Read(2000);
            if (ev.kind != rpc::StreamEventKind::Data) break;  // 客户端 DONE → 结束
            all += ev.payload;
        }
        co_await ctx.writer->Write("sum:" + all);
        co_await ctx.writer->Finish();  // 显式 Finish
    };
    server.Register("test.Stream", "ClientStream", std::move(mh));

    std::string out;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = rpc_client_stream_client(server.port(), &out, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(out == "sum:abcd", "客户端流汇总响应正确");
}

// ════════════ 9. 双向流（客户端写/读交错，服务端逐条回显）════════════

// 客户端协程：开流 → 写 x → 读回显 → 写 y → 读回显 → 半关闭 → 读到 Done
static coro::Task<void> rpc_bidi_client(int port, std::vector<std::string>* out,
                                        std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    if (!co_await ch->Open("127.0.0.1", port, 3000)) {
        out->push_back("CONN_FAIL");
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    try {
        auto st = co_await ch->OpenStream("test.Stream", "Bidi", 3000);
        co_await st->Write("x");
        rpc::StreamEvent e1 = co_await st->Read(2000);
        if (e1.kind == rpc::StreamEventKind::Data) out->push_back(e1.payload);
        co_await st->Write("y");
        rpc::StreamEvent e2 = co_await st->Read(2000);
        if (e2.kind == rpc::StreamEventKind::Data) out->push_back(e2.payload);
        co_await st->CloseWrite();
        co_await st->Read(2000);  // 等对端 DONE（收尾，不校验内容）
    } catch (const rpc::RpcException& e) {
        out->push_back("ERR:" + std::string(e.what()));
    }
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_stream_bidi() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "双向流 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Stream;
    mh.stream = [](const rpc::ServerCallContext& ctx) -> coro::Task<void> {
        for (;;) {
            rpc::StreamEvent ev = co_await ctx.reader->Read(2000);
            if (ev.kind != rpc::StreamEventKind::Data) break;  // 客户端 DONE → 结束
            co_await ctx.writer->Write("echo:" + ev.payload);
        }
        co_await ctx.writer->Finish();
    };
    server.Register("test.Stream", "Bidi", std::move(mh));

    std::vector<std::string> out;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = rpc_bidi_client(server.port(), &out, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(out.size() == 2, "双向流收到 2 条回显");
    if (out.size() == 2) {
        CHECK(out[0] == "echo:x" && out[1] == "echo:y", "双向流回显顺序正确");
    }
}

// ════════════════ 10. 连接池：复用 / 并发上限 / 等待超时 ════════════════

// 借用者协程：Acquire 取连接 → 记录指针 → 借机做一次真实调用 → Release(healthy) 归还。
// done 仅计数不参与 stop：由 coordinator 统一等齐后 CloseAll 并 stop。
static coro::Task<void> pool_user(rpc::RpcConnectionPool* pool, int port, int i,
                                  rpc::RpcChannel** got, std::atomic<int>* active,
                                  std::atomic<int>* peak, std::atomic<int>* done) {
    auto ch = co_await pool->Acquire("127.0.0.1", port, 3000);
    if (!ch) {
        *got = nullptr;
        ++*done;
        co_return;
    }
    *got = ch.get();
    // 借用期间：活跃计数 +1，更新并发峰值（CAS 取历史最大值）
    int cur = active->fetch_add(1) + 1;
    int p = peak->load();
    while (cur > p && !peak->compare_exchange_weak(p, cur)) {}
    try {
        co_await ch->UnaryCall("test.Echo", "SayHello", "p" + std::to_string(i), 2000);
    } catch (const rpc::RpcException&) {
        // 借用期调用失败不判定测试失败（复用连接若恰好断线属正常现象）
    }
    active->fetch_sub(1);
    pool->Release(ch);
    ++*done;
    co_return;
}

// 协调协程：等全部借用者完成 → CloseAll 关闭池内空闲连接 → stop。
// 连接池测试的公共收尾：池析构前必须让 idle 连接的读循环退出。
static coro::Task<void> pool_coord(rpc::RpcConnectionPool* pool,
                                   std::atomic<int>* done, int expect,
                                   std::atomic<int>* remaining, int total_coros) {
    while (done->load() < expect) co_await coro::sleep_for(10);
    co_await pool->CloseAll();
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

// max=1：两个借用者顺序执行（第二个等第一个归还后再 Acquire），
// 应拿到同一个连接对象（空闲复用，而非新建）
static coro::Task<void> pool_reuse_first(rpc::RpcConnectionPool* pool, int port,
                                         rpc::RpcChannel** got, std::atomic<int>* done) {
    auto ch = co_await pool->Acquire("127.0.0.1", port, 3000);
    *got = ch ? ch.get() : nullptr;
    if (ch) pool->Release(ch);
    ++*done;
    co_return;
}
static coro::Task<void> pool_reuse_second(rpc::RpcConnectionPool* pool, int port,
                                          std::atomic<int>* first_done,
                                          rpc::RpcChannel** got, std::atomic<int>* done) {
    while (first_done->load() == 0) co_await coro::sleep_for(10);  // 等第一个归还
    auto ch = co_await pool->Acquire("127.0.0.1", port, 3000);
    *got = ch ? ch.get() : nullptr;
    if (ch) pool->Release(ch);
    ++*done;
    co_return;
}

static void test_pool_reuse() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "池复用 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Unary;
    mh.unary = [](std::string req, const rpc::ServerCallContext&) -> coro::Task<std::string> {
        co_return "echo:" + req;
    };
    server.Register("test.Echo", "SayHello", std::move(mh));

    auto pool = std::make_shared<rpc::RpcConnectionPool>(1);  // 并发上限 1
    rpc::RpcChannel* got[2] = {nullptr, nullptr};
    std::atomic<int> done{0}, remaining{0};
    constexpr int kCoros = 1;

    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> f = pool_reuse_first(pool.get(), server.port(), &got[0], &done);
    coro::Task<void> g = pool_reuse_second(pool.get(), server.port(), &done, &got[1], &done);
    coro::Task<void> c = pool_coord(pool.get(), &done, 2, &remaining, kCoros);
    loop.post(f.handle());
    loop.post(g.handle());
    loop.post(c.handle());
    loop.run();

    CHECK(got[0] != nullptr && got[1] == got[0], "max=1 顺序借用复用同一连接");
}

// max=3，6 个借用者并发：同时最多 3 个连接被持有（峰值 ≤ 3），
// 且因复用充分，全流程只创建恰好 3 个不同连接
static void test_pool_upper_bound() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "池上限 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Unary;
    mh.unary = [](std::string req, const rpc::ServerCallContext&) -> coro::Task<std::string> {
        co_return "echo:" + req;
    };
    server.Register("test.Echo", "SayHello", std::move(mh));

    constexpr int kUsers = 6, kMax = 3;
    auto pool = std::make_shared<rpc::RpcConnectionPool>(kMax);
    std::vector<rpc::RpcChannel*> got(kUsers, nullptr);
    std::atomic<int> active{0}, peak{0}, done{0}, remaining{0};
    constexpr int kCoros = 1;

    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    for (int i = 0; i < kUsers; ++i) {
        coro::Task<void> u = pool_user(pool.get(), server.port(), i, &got[i],
                                       &active, &peak, &done);
        loop.post(u.handle());
    }
    coro::Task<void> c = pool_coord(pool.get(), &done, kUsers, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(done == kUsers, "6 个借用者全部完成");
    bool all_ok = true;
    for (int i = 0; i < kUsers; ++i) if (got[i] == nullptr) { all_ok = false; break; }
    CHECK(all_ok, "6 个借用者都拿到连接");
    CHECK(peak <= kMax, "并发借用峰值不超过上限 3");
    // 去重统计不同连接数：应恰好 = 上限（新建 3 个后全部复用）
    std::vector<rpc::RpcChannel*> uniq;
    for (int i = 0; i < kUsers; ++i) {
        if (std::find(uniq.begin(), uniq.end(), got[i]) == uniq.end()) uniq.push_back(got[i]);
    }
    CHECK(uniq.size() == static_cast<size_t>(kMax), "全程只创建 3 个连接（复用充分）");
}

// max=1：占住唯一连接 200ms，第二个 Acquire(80ms) 应超时返回空；
// 释放后第三个 Acquire 应成功
static coro::Task<void> pool_hold(rpc::RpcConnectionPool* pool, int port,
                                  std::atomic<int>* done, std::atomic<bool>* released) {
    auto ch = co_await pool->Acquire("127.0.0.1", port, 3000);
    CHECK(ch != nullptr, "池持有者拿到连接");
    co_await coro::sleep_for(200);  // 占住连接：让第二个借用者超时
    pool->Release(ch);
    *released = true;
    ++*done;
    co_return;
}
static coro::Task<void> pool_timed_waiter(rpc::RpcConnectionPool* pool, int port,
                                          bool* timed_out, std::atomic<int>* done) {
    auto ch = co_await pool->Acquire("127.0.0.1", port, 80);  // 80ms 超时（小于占位 200ms）
    *timed_out = (ch == nullptr);
    if (ch) pool->Release(ch);
    ++*done;
    co_return;
}
static coro::Task<void> pool_late(rpc::RpcConnectionPool* pool, int port,
                                  std::atomic<bool>* released, std::atomic<int>* done) {
    while (!released->load()) co_await coro::sleep_for(10);  // 等 holder 释放
    auto ch = co_await pool->Acquire("127.0.0.1", port, 3000);
    CHECK(ch != nullptr, "释放后池可再次借出");
    if (ch) pool->Release(ch);
    ++*done;
    co_return;
}

static void test_pool_timeout() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "池超时 server 监听成功");

    rpc::MethodHandler mh;
    mh.type = rpc::MethodHandler::Type::Unary;
    mh.unary = [](std::string req, const rpc::ServerCallContext&) -> coro::Task<std::string> {
        co_return "echo:" + req;
    };
    server.Register("test.Echo", "SayHello", std::move(mh));

    auto pool = std::make_shared<rpc::RpcConnectionPool>(1);
    bool timed_out = false;
    std::atomic<int> done{0}, remaining{0};
    std::atomic<bool> released{false};
    constexpr int kCoros = 1;

    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> h = pool_hold(pool.get(), server.port(), &done, &released);
    coro::Task<void> w = pool_timed_waiter(pool.get(), server.port(), &timed_out, &done);
    coro::Task<void> l = pool_late(pool.get(), server.port(), &released, &done);
    coro::Task<void> c = pool_coord(pool.get(), &done, 3, &remaining, kCoros);
    loop.post(h.handle());
    loop.post(w.handle());
    loop.post(l.handle());
    loop.post(c.handle());
    loop.run();

    CHECK(timed_out, "占满时 Acquire(80ms) 超时返回空");
}

// ════════════ 11. protoc 插件桩（greeter.rpc.h 生成代码往返）════════════

// 生成桩的业务实现：SayHello 回显问候，Chat 逐条回显（基类在 greeter 命名空间）
class TestGreeterImpl : public greeter::GreeterServiceBase {
public:
    // 一元：SayHello
    coro::Task<::greeter::HelloReply> SayHello(const ::greeter::HelloRequest& request) override {
        ::greeter::HelloReply rep;
        rep.set_message("hi " + request.name());
        co_return rep;
    }
    // 双向流：Chat
    coro::Task<void> Chat(rpc::ServerReaderWriter<::greeter::HelloRequest,
                                                  ::greeter::HelloReply>& stream) override {
        for (;;) {
            ::greeter::HelloRequest req;
            if (!co_await stream.Read(&req)) break;  // 客户端半关闭 → 结束
            ::greeter::HelloReply rep;
            rep.set_message("echo " + req.name());
            co_await stream.Write(rep);
        }
        co_await stream.Finish();
    }
};

// 客户端协程：Open → 一元 SayHello → 双向流 Chat → Close → stop。
// 全程使用 protoc 插件生成的 GreeterClient 桩，验证生成代码可编译且往返正确。
static coro::Task<void> codegen_client(int port, std::string* unary_out,
                                       std::vector<std::string>* chat_out,
                                       std::atomic<int>* remaining, int total_coros) {
    auto ch = std::make_shared<rpc::RpcChannel>();
    if (!co_await ch->Open("127.0.0.1", port, 3000)) {
        *unary_out = "CONN_FAIL";
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    greeter::GreeterClient client(*ch);
    try {
        // 一元：SayHello
        ::greeter::HelloRequest req;
        req.set_name("World");
        ::greeter::HelloReply rep = co_await client.SayHello(req, 3000);
        *unary_out = rep.message();

        // 双向流：Chat（连发 3 条，逐条收回显）
        auto st = co_await client.Chat(3000);
        for (const char* n : {"a", "b", "c"}) {
            ::greeter::HelloRequest r;
            r.set_name(n);
            co_await st->Write(r);
        }
        co_await st->CloseWrite();
        for (;;) {
            rpc::StreamEvent ev = co_await st->Read(2000);
            if (ev.kind != rpc::StreamEventKind::Data) break;  // Done / Timeout 均结束
            ::greeter::HelloReply hr;
            if (hr.ParseFromString(ev.payload)) chat_out->push_back(hr.message());
        }
    } catch (const rpc::RpcException& e) {
        *unary_out = "ERR:" + std::string(e.what());
    }
    ch->Close();
    while (!ch->read_done()) co_await coro::sleep_for(10);
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

static void test_greeter_codegen() {
    coro::EventLoop loop;
    rpc::RpcServer server(loop);
    CHECK(server.Start("127.0.0.1", 0), "codegen server 监听成功");

    TestGreeterImpl impl;
    greeter::RegisterGreeterService(server, impl);  // 生成代码：注册全部方法

    std::string unary_out;
    std::vector<std::string> chat_out;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> s = server.Serve();
    loop.post(s.handle());
    coro::Task<void> c = codegen_client(server.port(), &unary_out, &chat_out, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(unary_out == "hi World", "codegen 一元 SayHello 往返正确");
    CHECK(chat_out.size() == 3, "codegen 双向流收到 3 条回显");
    if (chat_out.size() == 3) {
        CHECK(chat_out[0] == "echo a" && chat_out[1] == "echo b" && chat_out[2] == "echo c",
              "codegen 双向流回显顺序正确");
    }
}

// ════════════ 16. RpcClient 门面（发现 + 连接池 + 重试）════════════════

// 门面客户端协程：注册实例 → CallUnary 两次（验证复用）→ CloseAll → 注销 → stop
static coro::Task<void> facades_client(int reg_port, int gport, std::string* out1,
                                       std::string* out2, std::atomic<int>* remaining,
                                       int total_coros) {
    // 1) 向 registry 注册 greeter 实例（业务服务端职责，测试里手动模拟）
    auto rch = std::make_shared<rpc::RpcChannel>();
    if (!co_await rch->Open("127.0.0.1", reg_port, 3000)) {
        *out1 = "REG_CONN_FAIL";
        if (++*remaining == total_coros) coro::EventLoop::current().stop();
        co_return;
    }
    rpc::registry::RegistryClient reg(*rch);
    rpc::registry::RegisterRequest rr;
    rpc::registry::Instance* inst = rr.mutable_instance();
    inst->set_service_name("greeter.Greeter");
    inst->set_host("127.0.0.1");
    inst->set_port(static_cast<std::uint32_t>(gport));
    inst->set_lease_seconds(60);
    rpc::registry::RegisterReply rrep = co_await reg.Register(rr, 3000);
    const std::string id = rrep.instance_id();
    CHECK(!id.empty(), "RpcClient 测试：实例注册成功");

    // 2) 门面：CallUnary 自动发现 + 调用；连续两次验证连接池复用
    rpc::RpcClient client("127.0.0.1", reg_port);
    greeter::HelloRequest req;
    req.set_name("World");
    try {
        std::string bytes = co_await client.CallUnary(
            "greeter.Greeter", "SayHello", req.SerializeAsString(), 3000);
        greeter::HelloReply rep;
        if (rep.ParseFromString(bytes)) *out1 = rep.message();

        bytes = co_await client.CallUnary("greeter.Greeter", "SayHello",
                                          req.SerializeAsString(), 3000);
        greeter::HelloReply rep2;
        if (rep2.ParseFromString(bytes)) *out2 = rep2.message();
    } catch (const rpc::RpcException& e) {
        *out1 = "ERR:" + std::string(e.what());
    }
    co_await client.CloseAll();

    // 3) 清理：注销实例
    rpc::registry::DeregisterRequest dq;
    dq.set_instance_id(id);
    (void)co_await reg.Deregister(dq, 3000);
    rch->Close();
    while (!rch->read_done()) co_await coro::sleep_for(10);
    if (++*remaining == total_coros) coro::EventLoop::current().stop();
    co_return;
}

// 同 loop 起 registry + greeter 两个 server，验证 RpcClient 门面全链路
static void test_rpc_client_facade() {
    coro::EventLoop loop;
    rpc::RpcServer reg_server(loop);
    CHECK(reg_server.Start("127.0.0.1", 0), "门面测试：registry server 监听成功");
    rpc::RegistryService registry;
    rpc::registry::RegisterRegistryService(reg_server, registry);  // 生成代码：注册中心方法

    rpc::RpcServer gserver(loop);
    CHECK(gserver.Start("127.0.0.1", 0), "门面测试：greeter server 监听成功");
    TestGreeterImpl impl;
    greeter::RegisterGreeterService(gserver, impl);

    std::string out1, out2;
    std::atomic<int> remaining{0};
    constexpr int kCoros = 1;
    coro::Task<void> rs = reg_server.Serve();
    coro::Task<void> gs = gserver.Serve();
    loop.post(rs.handle());
    loop.post(gs.handle());
    coro::Task<void> c = facades_client(reg_server.port(), gserver.port(),
                                        &out1, &out2, &remaining, kCoros);
    loop.post(c.handle());
    loop.run();

    CHECK(out1 == "hi World", "RpcClient 门面一元调用往返正确");
    CHECK(out2 == "hi World", "RpcClient 门面第二次调用正确（复用池连接）");
}

int main() {
    rpc::Init();  // 屏蔽 SIGPIPE：超时测试中对端关闭后晚到写不崩进程
    std::printf("== test_frame_encode_roundtrip ==\n"); std::fflush(stdout);
    test_frame_encode_roundtrip();
    std::printf("== test_frame_len_prefix_bigendian ==\n"); std::fflush(stdout);
    test_frame_len_prefix_bigendian();
    std::printf("== test_frame_encode_empty ==\n"); std::fflush(stdout);
    test_frame_encode_empty();
    std::printf("== test_frame_oversize_reject ==\n"); std::fflush(stdout);
    test_frame_oversize_reject();
    std::printf("== test_write_lock_concurrent ==\n"); std::fflush(stdout);
    test_write_lock_concurrent();
    std::printf("== test_write_lock_timeout ==\n"); std::fflush(stdout);
    test_write_lock_timeout();
    std::printf("== test_unary_roundtrip ==\n"); std::fflush(stdout);
    test_unary_roundtrip();
    std::printf("== test_channel_mux ==\n"); std::fflush(stdout);
    test_channel_mux();
    std::printf("== test_unary_timeout ==\n"); std::fflush(stdout);
    test_unary_timeout();
    std::printf("== test_stream_server_streaming ==\n"); std::fflush(stdout);
    test_stream_server_streaming();
    std::printf("== test_stream_client_streaming ==\n"); std::fflush(stdout);
    test_stream_client_streaming();
    std::printf("== test_stream_bidi ==\n"); std::fflush(stdout);
    test_stream_bidi();
    std::printf("== test_pool_reuse ==\n"); std::fflush(stdout);
    test_pool_reuse();
    std::printf("== test_pool_upper_bound ==\n"); std::fflush(stdout);
    test_pool_upper_bound();
    std::printf("== test_pool_timeout ==\n"); std::fflush(stdout);
    test_pool_timeout();
    std::printf("== test_greeter_codegen ==\n"); std::fflush(stdout);
    test_greeter_codegen();
    std::printf("== test_rpc_client_facade ==\n"); std::fflush(stdout);
    test_rpc_client_facade();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
