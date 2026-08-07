// net 层单元测试：断言宏 + 各组件测试函数（随任务追加）
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "coro/event_loop.h"
#include "coro/task.h"
#include "coro/awaiter.h"
#include "net/tcp_stream.h"
#include "net/tcp_listener.h"
#include "net/resolver.h"
#include "net/tls_context.h"
#include "net/tls_stream.h"
#include "net/buffered_reader.h"
#include "net/signal_watcher.h"
#include "net/when_all.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <cstring>
#include <csignal>
#include <tuple>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) { ++g_pass; } else {                                          \
            ++g_fail; std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

// 两个进程内 socketpair，一端包成 TcpStream 用协程读写，另一端裸 fd 对照
static int g_peer = -1;             // 裸 fd（对照端）
static std::atomic<int> g_read_ok{0}, g_read_n{0};
static std::atomic<int> g_weof_ok{0};

static coro::Task<void> tcp_read_task() {
    net::TcpStream s(g_peer);       // 接管对照端 fd（测试内不再直接 close 它）
    char buf[16];
    auto r = co_await s.read_some(buf, sizeof(buf), 2000);
    if (r.ok()) { g_read_n = (int)r.bytes; g_read_ok = 1; }
    coro::EventLoop::current().stop();
}
static coro::Task<void> tcp_read_eof_task() {
    net::TcpStream s(g_peer);
    char buf[16];
    auto r = co_await s.read_some(buf, sizeof(buf), 2000);
    if (r.err == net::IoError::Eof) g_weof_ok = 1;
    coro::EventLoop::current().stop();
}
// 注：write_all 测试用命名协程函数而非 [&] 捕获的协程 lambda——GCC 13 上
// [&] 协程 lambda 的闭包在语句结束时销毁，事件循环延迟 resume 会访问悬垂闭包
// （ASan 报 stack-use-after-scope）。参数按值/引用进帧，无闭包生命周期问题。
static coro::Task<void> tcp_write_task(int wfd, const std::string& data, std::atomic<int>* wrote) {
    net::TcpStream s(wfd);
    *wrote = (co_await s.write_all(data)) ? 1 : -1;
    coro::EventLoop::current().stop();
}
static coro::Task<void> tcp_writev_task(int wfd, const std::string& a,
                                        const std::string& b, std::atomic<int>* wrote) {
    net::TcpStream s(wfd);
    *wrote = (co_await s.writev_all({a, b})) ? 1 : -1;
    coro::EventLoop::current().stop();
}

// ── TLS 用例：net::TlsStream 握手 + 加密读写回环 ──
// 服务端在事件循环上跑 net::TlsStream（非阻塞协程），客户端在主线程用阻塞
// OpenSSL SSL_connect 连上，验证握手、ALPN(h2) 与 "abc"→"pong" 回环。
static std::atomic<int> g_tls_ok{0}, g_tls_echo{0};
static int g_tls_fd = -1;             // 服务端 accept 出的非阻塞 fd
static SSL_CTX* g_tls_ctx = nullptr;

static coro::Task<void> tls_server_task()
{
    net::TcpStream tcp(g_tls_fd);
    net::TlsStream ss(std::move(tcp), g_tls_ctx);
    auto hs = co_await ss.handshake(5000);
    if (!hs.ok()) { g_tls_ok = -1; coro::EventLoop::current().stop(); co_return; }
    char b[8];
    auto r = co_await ss.read_some(b, sizeof(b), 2000);
    if (r.ok() && r.bytes == 3) {
        bool w = co_await ss.write_all("pong", 2000);
        g_tls_echo = w ? 1 : -1;
    }
    g_tls_ok = 1;
    // 半关闭等待：读直到对端 close_notify，确保 "pong" 已送达再销毁连接。
    // 客户端 SSL_shutdown 会触发本读返回 Eof；即使未到，2s 超时后也会放行。
    co_await ss.read_some(b, sizeof(b), 2000);
    coro::EventLoop::current().stop();
}

// 主线程阻塞客户端：握手 + 发送 "abc" + 读回 "pong"，并校验 ALPN h2。
static bool tls_client_roundtrip(int cfd)
{
    struct timeval tv{5, 0};   // 防死锁兜底超时
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
    if (!cctx) return false;
    SSL* cssl = SSL_new(cctx);
    if (!cssl) { SSL_CTX_free(cctx); return false; }
    SSL_set_fd(cssl, cfd);

    static const unsigned char kAlpn[] = {2, 'h', '2'};   // 广告 h2
    SSL_set_alpn_protos(cssl, kAlpn, sizeof(kAlpn));

    bool ok = false;
    if (SSL_connect(cssl) == 1) {
        bool h2 = net::TlsContext::IsHttp2(cssl);
        if (SSL_write(cssl, "abc", 3) == 3) {
            char rbuf[8];
            int n = SSL_read(cssl, rbuf, sizeof(rbuf));
            ok = (h2 && n == 4 && std::memcmp(rbuf, "pong", 4) == 0);
        }
    }
    SSL_shutdown(cssl);   // 发送 close_notify，服务端读到 Eof 后收尾
    SSL_free(cssl);
    SSL_CTX_free(cctx);
    return ok;
}

static void test_tls()
{
    // 证书由 openssl 命令一次性生成（test/certs/server.key 被 gitignore，
    // 仅随仓库提交 .crt；新克隆机器需本地重新生成 key）。测试可能在仓库
    // 根目录或 build/ 下运行，两种相对路径都试一下。
    const char* cert = "test/certs/server.crt";
    const char* key = "test/certs/server.key";
    if (access(cert, F_OK) != 0 || access(key, F_OK) != 0) {
        cert = "../test/certs/server.crt";
        key = "../test/certs/server.key";
    }
    if (access(cert, F_OK) != 0 || access(key, F_OK) != 0) {
        std::printf("SKIP: TLS 证书缺失，请运行 openssl 生成 test/certs/server.{crt,key}\n");
        return;
    }

    net::TlsContext sctx;
    if (!sctx.Load(cert, key)) {
        CHECK(false, "TlsContext::Load 自签证书");
        return;
    }
    g_tls_ctx = sctx.NativeContext();

    // 监听临时端口
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (lfd < 0 ||
        bind(lfd, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 4) != 0) {
        CHECK(false, "TLS 监听 socket 创建");
        if (lfd >= 0) ::close(lfd);
        return;
    }
    socklen_t alen = sizeof(addr);
    getsockname(lfd, (sockaddr*)&addr, &alen);
    unsigned short port = ntohs(addr.sin_port);

    // 先建 TCP 连接，再 accept 出服务端 fd（设非阻塞）；随后客户端在同一
    // 连接上做阻塞 SSL_connect，服务端在事件循环上做非阻塞 SSL_accept。
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in caddr{};
    caddr.sin_family = AF_INET;
    caddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &caddr.sin_addr);
    if (cfd < 0 || connect(cfd, (sockaddr*)&caddr, sizeof(caddr)) != 0) {
        CHECK(false, "TLS 客户端 TCP 连接");
        if (cfd >= 0) ::close(cfd);
        ::close(lfd);
        return;
    }
    g_tls_fd = accept(lfd, nullptr, nullptr);
    CHECK(g_tls_fd >= 0, "TLS 服务端 accept");
    ::close(lfd);
    if (g_tls_fd >= 0)
        fcntl(g_tls_fd, F_SETFL, fcntl(g_tls_fd, F_GETFL, 0) | O_NONBLOCK);

    g_tls_ok = 0; g_tls_echo = 0;
    coro::EventLoop loop;
    coro::Task<void> t = tls_server_task();
    loop.post(t.handle());
    std::thread loop_thread([&] { loop.run(); });

    bool ok = tls_client_roundtrip(cfd);
    CHECK(ok, "TLS 客户端握手 + ALPN(h2) + 加密回环 ('abc'→'pong')");
    CHECK(g_tls_ok == 1, "服务端 TLS 握手/读/写成功");
    CHECK(g_tls_echo == 1, "服务端回写 'pong' 成功");

    loop.stop();
    loop_thread.join();
    ::close(cfd);
}

// ── 监听/连接用例：TcpListener::open/accept + net::connect ──
// 服务端监听协程在后台 loop 上跑：open(0) → 通知主线程取端口 → accept(3s)
// → 读 2 字节；客户端在主线程的独立小 loop 上 net::connect + write_all("ok")。
// 注：g_listen_fd 由 loop 线程写、主线程读，靠 g_listen_ready 的
// release/acquire 序（std::atomic）保证可见性。
static std::atomic<int> g_listen_ready{0};
static std::atomic<int> g_accepted{0};
static std::atomic<int> g_client_ok{0};
static int g_listen_fd = -1;

static coro::Task<void> listener_task()
{
    net::TcpListener ln;
    CHECK(ln.open("127.0.0.1", 0, false), "listener open");   // port=0 → 系统分配
    g_listen_fd = ln.fd();
    g_listen_ready = 1;                                        // 主线程据此 getsockname
    net::TcpStream c;
    auto r = co_await ln.accept(c, 3000);
    if (r.ok()) {
        g_accepted = 1;
        char buf[4];
        auto rr = co_await c.read_some(buf, sizeof(buf), 1000);
        if (rr.ok() && rr.bytes == 2) g_accepted = 2;
    }
    coro::EventLoop::current().stop();
}

// 客户端：connect 成功后写 "ok"；结果写入 g_client_ok 后停掉所在 loop
static coro::Task<void> client_task(std::string_view host, uint16_t port)
{
    auto s = co_await net::connect(host, port, 3000);
    if (!s) { g_client_ok = -1; coro::EventLoop::current().stop(); co_return; }
    bool w = co_await s->write_all("ok", 2000);
    g_client_ok = w ? 1 : -1;
    coro::EventLoop::current().stop();
}

// 连接被拒/超时用例：连 127.0.0.1:1（通常 ECONNREFUSED），应返回 nullptr
static std::atomic<int> g_refused{0};   // 0=未决, 1=nullptr, -1=意外连接成功
static coro::Task<void> refused_task(std::string_view host, uint16_t port)
{
    auto s = co_await net::connect(host, port, 3000);
    g_refused = s ? -1 : 1;
    coro::EventLoop::current().stop();
}

static void test_listen_connect()
{
    g_listen_ready = 0; g_accepted = 0; g_client_ok = 0; g_listen_fd = -1;

    coro::EventLoop listen_loop;
    coro::Task<void> lt = listener_task();
    listen_loop.post(lt.handle());
    std::thread lt_thread([&] { listen_loop.run(); });

    // 等待后台监听就绪（fd 已赋值且已 listen），带超时兜底防死等
    int spins = 0;
    while (g_listen_ready == 0 && spins < 10000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++spins;
    }
    CHECK(g_listen_ready == 1 && g_listen_fd >= 0, "listener 后台就绪");

    if (g_listen_fd >= 0) {
        sockaddr_in laddr{};
        socklen_t llen = sizeof(laddr);
        CHECK(getsockname(g_listen_fd, (sockaddr*)&laddr, &llen) == 0, "getsockname 取端口");
        uint16_t port = ntohs(laddr.sin_port);
        CHECK(port != 0, "系统分配端口非 0");

        // 客户端在独立小 loop 上 connect + 写 "ok"（client_task 完成后自停）
        coro::EventLoop conn_loop;
        coro::Task<void> ct = client_task("127.0.0.1", port);
        conn_loop.post(ct.handle());
        conn_loop.run();
        CHECK(g_client_ok == 1, "connect + 写 'ok' 成功");
    }

    // 等服务端 accept 并读回 2 字节（listener_task 完成后也会自停）
    int spins2 = 0;
    while (g_accepted != 2 && spins2 < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++spins2;
    }
    listen_loop.stop();
    lt_thread.join();
    CHECK(g_accepted == 2, "服务端 accept + 读回 2 字节");
}

static void test_connect_refused()
{
    g_refused = 0;
    coro::EventLoop loop;
    coro::Task<void> t = refused_task("127.0.0.1", 1);
    loop.post(t.handle());
    loop.run();       // refused_task 完成后自停
    CHECK(g_refused == 1, "connect 被拒返回 nullptr");
}

static void test_resolve()
{
    auto eps = net::resolve("localhost", 80);
    CHECK(!eps.empty(), "resolve localhost 非空");
    auto eps2 = net::resolve("127.0.0.1", 8080);
    CHECK(!eps2.empty(), "resolve 127.0.0.1 非空");
    if (!eps2.empty()) {
        CHECK(eps2[0].host == "127.0.0.1", "resolve 保留数字 IP");
        CHECK(eps2[0].port == 8080, "resolve 端口保持");
    }
}

// ── BufferedReader 用例：read_until（跨读分隔符）/ read_exact / buffered ──
// 核心验证：read_until 对"分隔符横跨两次 TCP 分段"的处理。任务简报的 read_until
// 在未命中时把 buf_ 整体清出，分隔符被拆开时永不匹配（读至 EOF）；本实现保留
// 末尾 delim.size()-1 字节作为跨读候选（见 net/buffered_reader.cpp 注释）。
static std::atomic<int> g_br_ok{0};
static int g_br_fd = -1;                 // 包成 TcpStream 的对照端
static std::string g_br_out;

static coro::Task<void> br_split_task() {
    net::TcpStream s(g_br_fd);
    net::BufferedReader br(s);
    // 1) 跨读分隔符：chunk1 以 "\r\n" 结尾（"\r\n\r\n" 的前 2 字节），chunk2 补上
    //    剩余 "\r\n"——完整分隔符必须靠保留的候选字节拼接才能命中
    auto r = co_await br.read_until("\r\n\r\n", g_br_out);
    if (!r.ok() || g_br_out != "GET / HTTP/1.1\r\nHost: x") {
        g_br_ok = -1; coro::EventLoop::current().stop(); co_return;
    }
    // 2) 分隔符已消费，其后的 "body" 留在缓冲中未消费（surplus 可被继续发现）
    if (br.buffered() != "body") { g_br_ok = -2; coro::EventLoop::current().stop(); co_return; }
    // 3) read_exact：先消费缓冲中 "body"（4 字节），再从流中补读 "XY"（2 字节）
    std::string exact;
    auto re = co_await br.read_exact(6, exact);
    if (!re.ok() || exact != "bodyXY") { g_br_ok = -3; coro::EventLoop::current().stop(); co_return; }
    // 4) 缓冲已被消费尽；同一 BufferedReader 再次 read_until 应越过已消费的
    //    分隔符继续工作（新写入的第二个头部块命中）
    std::string out2;
    auto r2 = co_await br.read_until("\r\n\r\n", out2);
    if (!r2.ok() || out2 != "POST /api HTTP/1.1\r\nContent-Length: 0") {
        g_br_ok = -4; coro::EventLoop::current().stop(); co_return;
    }
    g_br_ok = 1;
    coro::EventLoop::current().stop();
}

static void test_buffered_reader_split() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair br 跨读创建");
    g_br_fd = sv[0];
    fcntl(g_br_fd, F_SETFL, fcntl(g_br_fd, F_GETFL, 0) | O_NONBLOCK);
    int wfd = sv[1];

    g_br_ok = 0; g_br_out.clear();
    coro::EventLoop loop;
    coro::Task<void> t = br_split_task();
    loop.post(t.handle());
    std::thread writer([&] {
        const char* c1 = "GET / HTTP/1.1\r\nHost: x\r\n";   // 结尾 "\r\n" 是分隔符前 2 字节
        const char* c2 = "\r\nbody";                        // 补全分隔符 + 4 字节 surplus
        const char* c3 = "XYPOST /api HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
        ssize_t n1 = write(wfd, c1, std::strlen(c1));
        if (n1 != (ssize_t)std::strlen(c1)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ssize_t n2 = write(wfd, c2, std::strlen(c2));
        if (n2 != (ssize_t)std::strlen(c2)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ssize_t n3 = write(wfd, c3, std::strlen(c3));
        (void)n3;
    });
    loop.run();
    writer.join();
    CHECK(g_br_ok == 1, "read_until 跨读分隔符命中 + read_exact + 二次 read_until");
    ::close(wfd);   // sv[0] 已由 TcpStream 析构关闭
}

// 单次读内即命中分隔符（不分段），read_until 应直接返回
static std::atomic<int> g_br1_ok{0};
static int g_br1_fd = -1;
static std::string g_br1_out;

static coro::Task<void> br_single_task() {
    net::TcpStream s(g_br1_fd);
    net::BufferedReader br(s);
    auto r = co_await br.read_until("\r\n\r\n", g_br1_out);
    g_br1_ok = (r.ok() && g_br1_out == "GET / HTTP/1.1\r\nHost: x") ? 1 : -1;
    coro::EventLoop::current().stop();
}

static void test_buffered_reader_single() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair br 单读创建");
    g_br1_fd = sv[0];
    fcntl(g_br1_fd, F_SETFL, fcntl(g_br1_fd, F_GETFL, 0) | O_NONBLOCK);
    int wfd = sv[1];
    const char* msg = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(write(wfd, msg, std::strlen(msg)) == (ssize_t)std::strlen(msg), "单读预写完整头");
    g_br1_ok = 0; g_br1_out.clear();
    coro::EventLoop loop;
    coro::Task<void> t = br_single_task();
    loop.post(t.handle());
    loop.run();
    CHECK(g_br1_ok == 1, "单次读内 read_until 命中分隔符");
    ::close(wfd);
}

// read_exact 在缓冲 + 流仍未读满 n 字节时遇 EOF → 返回 IoError::Eof
static std::atomic<int> g_br2_ok{0};
static int g_br2_fd = -1;

static coro::Task<void> br_eof_task() {
    net::TcpStream s(g_br2_fd);
    net::BufferedReader br(s);
    std::string out;
    auto r = co_await br.read_exact(8, out);
    g_br2_ok = (r.err == net::IoError::Eof) ? 1 : -1;
    coro::EventLoop::current().stop();
}

static void test_buffered_reader_eof() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair br eof 创建");
    g_br2_fd = sv[0];
    fcntl(g_br2_fd, F_SETFL, fcntl(g_br2_fd, F_GETFL, 0) | O_NONBLOCK);
    int wfd = sv[1];
    CHECK(write(wfd, "abcde", 5) == 5, "eof 预写 5 字节");
    ::close(wfd);   // 关闭写端 → 读 5 字节后 EOF
    g_br2_ok = 0;
    coro::EventLoop loop;
    coro::Task<void> t = br_eof_task();
    loop.post(t.handle());
    loop.run();
    CHECK(g_br2_ok == 1, "read_exact 未满 n 遇 EOF 返回 IoError::Eof");
}

// ── SignalWatcher 冒烟：编译/链接/基本语义（优雅停机测试留到 Task 8）──
static void test_signal_watcher_smoke() {
    net::SignalWatcher sw;
    CHECK(sw.init({SIGUSR2}), "SignalWatcher::init 创建 signalfd");
    CHECK(sw.fd() >= 0, "SignalWatcher fd 非负");
    sw.close();
    CHECK(sw.fd() == -1, "SignalWatcher::close 后 fd 为 -1");
}

// ── when_all 用例：并发等待两个子任务（简报 Step 4 逐字）──
static coro::Task<int> wc_slow() { co_await coro::sleep_for(30); co_return 1; }
static coro::Task<int> wc_fast() { co_return 2; }
static std::atomic<int> g_when_ok{0};
static coro::Task<void> when_all_driver() {
    auto [a, b] = co_await net::when_all(wc_slow(), wc_fast());
    g_when_ok = (a == 1 && b == 2) ? 1 : 0;
    coro::EventLoop::current().stop();
}

static void test_when_all() {
    g_when_ok = 0;
    coro::EventLoop loop;
    coro::Task<void> t = when_all_driver();
    loop.post(t.handle());
    loop.run();
    CHECK(g_when_ok == 1, "when_all 并发两个子任务结果 {1,2}");
}

int main() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 创建");
    g_peer = sv[1];
    int fd = sv[0];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    // 1. 就绪读：预写数据，协程读回
    {
        coro::EventLoop loop;
        g_read_ok = 0; g_read_n = 0;
        CHECK(write(fd, "hello", 5) == 5, "预写 5 字节");
        coro::Task<void> t = tcp_read_task();
        loop.post(t.handle());
        loop.run();
        CHECK(g_read_ok == 1 && g_read_n == 5, "read_some 读回 5 字节");
        ::close(fd);   // 回收 sv[0]（sv[1] 已由 TcpStream 析构关闭）
    }
    // 2. EOF：对端关闭后 read_some 返回 Eof
    // 注意：case 1 的 TcpStream 已接管并关闭 sv[1]，故此处用全新 socketpair，
    // 避免 TcpStream 包装到被关闭的旧 fd 号上。
    {
        int sv2[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0, "socketpair2 创建");
        g_peer = sv2[1];
        ::close(sv2[0]);  // 关闭写入端 → 对端 EOF
        coro::EventLoop loop;
        g_weof_ok = 0;
        coro::Task<void> t = tcp_read_eof_task();
        loop.post(t.handle());
        loop.run();
        CHECK(g_weof_ok == 1, "对端关闭后 read_some 返回 Eof");
    }
    // 3. write_all：大块写入（8KB），对端读净
    {
        int sv2[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0, "socketpair2 创建");
        std::string big(8192, 'x');
        std::atomic<int> wrote{0};
        coro::EventLoop loop;
        int wfd = sv2[0], rfd = sv2[1];   // wfd 归 TcpStream 管，rfd 由测试线程裸读
        coro::Task<void> w = tcp_write_task(wfd, big, &wrote);
        loop.post(w.handle());
        std::thread reader([&] {
            std::string got; char b[4096]; ssize_t n;
            while ((n = read(rfd, b, sizeof(b))) > 0) got.append(b, n);
            CHECK((int)got.size() == 8192, "write_all 8KB 全部到达对端");
        });
        loop.run();
        reader.join();
        CHECK(wrote == 1, "write_all 返回 true");
        ::close(rfd);   // wfd 已由 TcpStream 析构关闭
    }
    // 3b. writev_all：两段 scatter 写（4KB+4KB），对端按序收到拼接
    {
        int sv3[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv3) == 0, "socketpair3 创建");
        std::string a(4096, 'a'), b(4096, 'b');
        std::atomic<int> wrote{0};
        coro::EventLoop loop;
        int wfd = sv3[0], rfd = sv3[1];   // wfd 归 TcpStream 管，rfd 由测试线程裸读
        coro::Task<void> w = tcp_writev_task(wfd, a, b, &wrote);
        loop.post(w.handle());
        std::thread reader([&] {
            std::string got; char buf[4096]; ssize_t n;
            while ((n = read(rfd, buf, sizeof(buf))) > 0) got.append(buf, n);
            CHECK((int)got.size() == 8192, "writev_all 8KB 全部到达对端");
            CHECK(got.substr(0, 4096) == a, "writev_all 段 1 顺序正确");
            CHECK(got.substr(4096) == b, "writev_all 段 2 顺序正确");
        });
        loop.run();
        reader.join();
        CHECK(wrote == 1, "writev_all 返回 true");
        ::close(rfd);   // wfd 已由 TcpStream 析构关闭
    }
    // 4. TLS：握手 + 加密读写回环（net::TlsStream）
    test_tls();
    // 5. 监听/连接：TcpListener::accept + net::connect 回环
    test_listen_connect();
    // 6. connect 被拒/超时：127.0.0.1:1 → nullptr
    test_connect_refused();
    // 7. resolve：主机名/数字 IP 解析
    test_resolve();
    // 8. BufferedReader：read_until 跨读分隔符 / 单读命中 / read_exact EOF
    test_buffered_reader_split();
    test_buffered_reader_single();
    test_buffered_reader_eof();
    // 9. SignalWatcher：编译/链接/基本语义冒烟
    test_signal_watcher_smoke();
    // 10. when_all：并发等待两个子任务
    test_when_all();

    std::printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
