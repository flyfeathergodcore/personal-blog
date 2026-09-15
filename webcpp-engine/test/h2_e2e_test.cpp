// HTTP/2 端到端测试：真实 TLS 连接上的 H2Session，由 nghttp2 模拟对端。
// 覆盖小发送窗口下的 DATA 分帧、发送账本扣减、等待 WINDOW_UPDATE 与续发。
#include "server/h2_session.hpp"
#include "net/tcp_stream.h"
#include "net/tls_stream.h"
#include "protocol/region_pool.hpp"
#include "router/router.hpp"
#include "middleware/middleware.hpp"
#include "handler/request_handler.hpp"
#include "protocol/response.hpp"
#include "coro/event_loop.h"

#include <nghttp2/nghttp2.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr size_t kBodySize = 4096;
constexpr int32_t kPeerWindow = 1024;

int g_pass = 0;
int g_fail = 0;
#define CHECK(condition, message) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL: %s (%s:%d)\\n", message, __FILE__, __LINE__); } \
} while (0)

class LargeBodyHandler final : public RequestHandler {
public:
    Response Handle(const Context& context) override {
        auto& region = *context.Pool();
        Response response(200, region);
        response.Header("content-type", "application/octet-stream");
        response.Header("content-length", kBodySize);
        response.EndHeaders();
        region.Write(std::string(kBodySize, 'x'));
        return response;
    }
};

struct TempCertificate {
    std::string cert_path;
    std::string key_path;

    ~TempCertificate() {
        if (!cert_path.empty()) ::unlink(cert_path.c_str());
        if (!key_path.empty()) ::unlink(key_path.c_str());
    }

    bool Create() {
        char cert_template[] = "/tmp/webcpp-h2-cert-XXXXXX";
        char key_template[] = "/tmp/webcpp-h2-key-XXXXXX";
        const int cert_fd = ::mkstemp(cert_template);
        const int key_fd = ::mkstemp(key_template);
        if (cert_fd < 0 || key_fd < 0) {
            if (cert_fd >= 0) ::close(cert_fd);
            if (key_fd >= 0) ::close(key_fd);
            return false;
        }
        ::close(cert_fd);
        ::close(key_fd);
        cert_path = cert_template;
        key_path = key_template;

        EVP_PKEY_CTX* key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY* key = nullptr;
        X509* cert = nullptr;
        bool ok = key_ctx && EVP_PKEY_keygen_init(key_ctx) == 1
            && EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx, 2048) == 1
            && EVP_PKEY_keygen(key_ctx, &key) == 1;
        if (ok) {
            cert = X509_new();
            ok = cert && X509_set_version(cert, 2) == 1
                && ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1
                && X509_gmtime_adj(X509_getm_notBefore(cert), 0)
                && X509_gmtime_adj(X509_getm_notAfter(cert), 24 * 60 * 60)
                && X509_set_pubkey(cert, key) == 1;
        }
        if (ok) {
            X509_NAME* subject = X509_get_subject_name(cert);
            ok = X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                             reinterpret_cast<const unsigned char*>("localhost"),
                                             -1, -1, 0) == 1
                && X509_set_issuer_name(cert, subject) == 1
                && X509_sign(cert, key, EVP_sha256()) != 0;
        }
        if (ok) {
            FILE* cert_file = std::fopen(cert_path.c_str(), "w");
            FILE* key_file = std::fopen(key_path.c_str(), "w");
            ok = cert_file && key_file
                && PEM_write_X509(cert_file, cert) == 1
                && PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            if (cert_file) std::fclose(cert_file);
            if (key_file) std::fclose(key_file);
        }
        X509_free(cert);
        EVP_PKEY_free(key);
        EVP_PKEY_CTX_free(key_ctx);
        return ok;
    }
};

struct ServerState {
    int fd = -1;
    SSL_CTX* tls_ctx = nullptr;
    Router* router = nullptr;
    MiddlewareManager* middleware = nullptr;
    RegionPool* region_pool = nullptr;
    std::atomic<bool>* handshake_ok = nullptr;
};

coro::Task<void> ServeH2(ServerState state) {
    net::TcpStream tcp(state.fd);
    net::TlsStream tls(std::move(tcp), state.tls_ctx);
    const auto handshake = co_await tls.handshake(5000);
    if (!handshake.ok()) {
        coro::EventLoop::current().stop();
        co_return;
    }
    state.handshake_ok->store(true, std::memory_order_release);
    auto session = std::make_shared<H2Session>(
        std::move(tls), *state.router, *state.middleware, state.region_pool);
    co_await session->Start();
    coro::EventLoop::current().stop();
}

struct ClientState {
    nghttp2_session* session = nullptr;
    std::vector<uint8_t> output;
    std::string body;
    int32_t stream_id = -1;
    bool got_200 = false;
    bool stream_closed = false;
    bool first_data_seen = false;
    size_t first_data_size = 0;
};

ssize_t ClientSend(nghttp2_session*, const uint8_t* data, size_t len, int, void* user_data) {
    auto* state = static_cast<ClientState*>(user_data);
    state->output.insert(state->output.end(), data, data + len);
    return static_cast<ssize_t>(len);
}

int OnHeader(nghttp2_session*, const nghttp2_frame*, const uint8_t* name, size_t name_len,
             const uint8_t* value, size_t value_len, uint8_t, void* user_data) {
    auto* state = static_cast<ClientState*>(user_data);
    if (std::string_view(reinterpret_cast<const char*>(name), name_len) == ":status"
        && std::string_view(reinterpret_cast<const char*>(value), value_len) == "200")
        state->got_200 = true;
    return 0;
}

int OnData(nghttp2_session* session, uint8_t, int32_t stream_id,
           const uint8_t* data, size_t len, void* user_data) {
    auto* state = static_cast<ClientState*>(user_data);
    if (!state->first_data_seen) {
        state->first_data_seen = true;
        state->first_data_size = len;
    }
    state->body.append(reinterpret_cast<const char*>(data), len);
    // 关闭 nghttp2 自动窗口策略后，按实际消费量生成流级和连接级更新。
    return nghttp2_session_consume(session, stream_id, len);
}

int OnClose(nghttp2_session*, int32_t, uint32_t, void* user_data) {
    static_cast<ClientState*>(user_data)->stream_closed = true;
    return 0;
}

bool FlushClient(SSL* ssl, ClientState& state) {
    if (state.output.empty()) return true;
    const auto expected = static_cast<int>(state.output.size());
    const int written = SSL_write(ssl, state.output.data(), expected);
    state.output.clear();
    return written == expected;
}

bool RunClient(int fd, ClientState& state) {
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = ctx ? SSL_new(ctx) : nullptr;
    if (!ssl) { SSL_CTX_free(ctx); return false; }
    SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
    SSL_set_fd(ssl, fd);
    static const unsigned char alpn[] = {2, 'h', '2'};
    SSL_set_alpn_protos(ssl, alpn, sizeof(alpn));
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); return false; }

    nghttp2_session_callbacks* callbacks = nullptr;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, ClientSend);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, OnHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, OnData);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, OnClose);
    nghttp2_option* options = nullptr;
    nghttp2_option_new(&options);
    nghttp2_option_set_no_auto_window_update(options, 1);
    const bool created = nghttp2_session_client_new3(
        &state.session, callbacks, &state, options, nullptr) == 0;
    nghttp2_option_del(options);
    nghttp2_session_callbacks_del(callbacks);
    if (!created) { SSL_free(ssl); SSL_CTX_free(ctx); return false; }

    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, kPeerWindow},
    };
    bool ok = nghttp2_submit_settings(state.session, NGHTTP2_FLAG_NONE, settings, 1) == 0
        && nghttp2_session_send(state.session) == 0
        && FlushClient(ssl, state);
    const char* names[] = {":method", ":scheme", ":authority", ":path"};
    const char* values[] = {"GET", "https", "localhost", "/large"};
    nghttp2_nv headers[4];
    for (size_t i = 0; i < 4; ++i) {
        headers[i] = {reinterpret_cast<uint8_t*>(const_cast<char*>(names[i])),
                      reinterpret_cast<uint8_t*>(const_cast<char*>(values[i])),
                      std::strlen(names[i]), std::strlen(values[i]), NGHTTP2_NV_FLAG_NONE};
    }
    state.stream_id = nghttp2_submit_headers(state.session,
        NGHTTP2_FLAG_END_STREAM | NGHTTP2_FLAG_END_HEADERS,
        -1, nullptr, headers, 4, nullptr);
    ok = ok && state.stream_id > 0 && nghttp2_session_send(state.session) == 0
        && FlushClient(ssl, state);

    std::vector<uint8_t> input(16 * 1024);
    for (int i = 0; ok && i < 20 && !state.stream_closed; ++i) {
        const int received = SSL_read(ssl, input.data(), static_cast<int>(input.size()));
        if (received <= 0) { ok = false; break; }
        ok = nghttp2_session_mem_recv(state.session, input.data(), received) >= 0
            && nghttp2_session_send(state.session) == 0
            && FlushClient(ssl, state);
    }

    nghttp2_session_del(state.session);
    state.session = nullptr;
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return ok;
}

void TestWindowResume() {
    TempCertificate certificate;
    CHECK(certificate.Create(), "生成临时自签 TLS 证书");
    if (g_fail != 0) return;

    SSL_CTX* server_ctx = SSL_CTX_new(TLS_server_method());
    CHECK(server_ctx != nullptr, "创建 TLS 服务端上下文");
    if (!server_ctx) return;
    static const unsigned char alpn[] = {2, 'h', '2'};
    SSL_CTX_set_alpn_select_cb(server_ctx,
        [](SSL*, const unsigned char** out, unsigned char* out_len,
           const unsigned char* in, unsigned int in_len, void*) {
            if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_len,
                                      alpn, sizeof(alpn), in, in_len)
                != OPENSSL_NPN_NEGOTIATED)
                return SSL_TLSEXT_ERR_NOACK;
            return SSL_TLSEXT_ERR_OK;
        }, nullptr);
    const bool loaded = SSL_CTX_use_certificate_chain_file(server_ctx, certificate.cert_path.c_str()) == 1
        && SSL_CTX_use_PrivateKey_file(server_ctx, certificate.key_path.c_str(), SSL_FILETYPE_PEM) == 1;
    CHECK(loaded, "加载临时 TLS 证书");
    if (!loaded) { SSL_CTX_free(server_ctx); return; }

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "创建 TLS socketpair");
    if (sockets[0] < 0) { SSL_CTX_free(server_ctx); return; }
    ::fcntl(sockets[0], F_SETFL, ::fcntl(sockets[0], F_GETFL, 0) | O_NONBLOCK);

    Router router;
    router.Get("/large", std::make_unique<LargeBodyHandler>());
    MiddlewareManager middleware;
    RegionPool pool;
    std::atomic<bool> handshake_ok{false};
    ServerState server_state{sockets[0], server_ctx, &router, &middleware, &pool, &handshake_ok};
    coro::EventLoop loop;
    auto server_task = ServeH2(server_state);
    loop.post(server_task.handle());
    std::thread server_thread([&] { loop.run(); });

    ClientState client;
    const bool client_ok = RunClient(sockets[1], client);
    CHECK(client_ok, "nghttp2 客户端完成 TLS/H2 会话");
    CHECK(handshake_ok.load(std::memory_order_acquire), "服务端协商 TLS 与 ALPN");
    CHECK(client.got_200, "收到 HTTP/2 200 响应头");
    CHECK(client.first_data_seen && client.first_data_size == kPeerWindow,
          "初次 DATA 严格受对端发送窗口限制");
    CHECK(client.body.size() == kBodySize, "WINDOW_UPDATE 后续发完整响应体");
    CHECK(client.stream_closed, "响应以 END_STREAM 结束");

    loop.stop();
    server_thread.join();
    ::close(sockets[1]);
    SSL_CTX_free(server_ctx);
}
}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    TestWindowResume();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
