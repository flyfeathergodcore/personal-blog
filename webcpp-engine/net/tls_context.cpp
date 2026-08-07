// net 层 TLS 上下文实现：裸 SSL_CTX*（无 asio），服务端方法 + ALPN + 会话缓存。
#include "net/tls_context.h"

#include <cstring>
#include <iostream>
#include <random>

#include <openssl/pem.h>

namespace net {

// 从 PEM 文件加载 DH 参数并设置到 SSL_CTX（供 TLS<=1.2 DHE 套件）
// 参数：ctx - 目标 SSL_CTX；dh_file - PEM 格式 DH 参数文件路径。成功返回 true
// ── 可选 DH 参数文件加载 ──
// OpenSSL 3.0 已移除 SSL_CTX_set_tmp_dh_file，改用 PEM_read_bio_DHparams 读取
// DH* 再经 SSL_CTX_set_tmp_dh 设置（保留给 TLS<=1.2 DHE 套件，兼容旧配置）。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static bool load_dh_file(SSL_CTX* ctx, const std::string& dh_file)
{
    BIO* bio = BIO_new_file(dh_file.c_str(), "r");
    if (!bio) return false;
    DH* dh = PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!dh) return false;
    int ok = SSL_CTX_set_tmp_dh(ctx, dh);
    DH_free(dh);
    return ok == 1;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ALPN 选择回调：按 RFC 7301 广告 "h2" 优先、"h1" 兜底。
// wire format：单字节长度 + 协议名，如 {2,'h','2'} 表示 "h2"。
// 参数：out/outlen - 输出选中协议；in/inlen - 客户端 ALPN 列表
static int alpn_select_cb(SSL* /*ssl*/,
                          const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen,
                          void* /*arg*/)
{
    static const unsigned char kH2[] = {2, 'h', '2'};
    static const unsigned char kH1[] = {2, 'h', '1'};
    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              kH2, sizeof(kH2), in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        if (SSL_select_next_proto((unsigned char**)out, outlen,
                                  kH1, sizeof(kH1), in, inlen) != OPENSSL_NPN_NEGOTIATED)
            return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

// 构造：创建服务端 SSL_CTX，配置安全默认项、ALPN 回调与 TLS 会话缓存
TlsContext::TlsContext()
{
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        std::cerr << "[tls] SSL_CTX_new 失败" << std::endl;
        return;
    }

    // 安全默认：禁用旧协议与易受攻击的特性（与 asio 版原选项对应）。
    // OpenSSL 3.0 中 SSL_OP_DEFAULT_WORKAROUNDS 已移除，等价于 SSL_OP_ALL；
    // SSL_OP_NO_SSLv2 / SSL_OP_SINGLE_DH_USE 在 3.0 为 0x0 保留宏（无操作）。
    SSL_CTX_set_options(ctx_,
        SSL_OP_ALL |
        SSL_OP_NO_SSLv2 |
        SSL_OP_NO_SSLv3 |
        SSL_OP_NO_TLSv1 |
        SSL_OP_NO_TLSv1_1 |
        SSL_OP_SINGLE_DH_USE);

    // ── ALPN ──
    SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, nullptr);

    // ── TLS 会话缓存 ──
    SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_SERVER);

    unsigned char session_id[32];
    const char* prefix = "webcpp-srv-1";
    std::memcpy(session_id, prefix, std::strlen(prefix));

    std::mt19937 rng(std::random_device{}());
    for (size_t i = std::strlen(prefix); i < sizeof(session_id); ++i)
        session_id[i] = static_cast<unsigned char>(rng() & 0xFF);

    SSL_CTX_set_session_id_context(ctx_, session_id, sizeof(session_id));
    SSL_CTX_set_num_tickets(ctx_, 1);
    SSL_CTX_set_timeout(ctx_, 300);
    SSL_CTX_sess_set_cache_size(ctx_, 1024);

    std::cout << "[tls] 会话缓存已启用 (max 1024, timeout 300s)" << std::endl;
}

// 析构：释放 SSL_CTX
TlsContext::~TlsContext()
{
    if (ctx_) SSL_CTX_free(ctx_);
}

// 检查指定 SSL 会话是否经 ALPN 协商出 "h2"
// 参数：ssl - 目标 SSL 会话。协商出 h2 返回 true
bool TlsContext::IsHttp2(SSL* ssl)
{
    if (!ssl) return false;
    const unsigned char* alpn = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    return (alpn && alpn_len == 2 && std::memcmp(alpn, "h2", 2) == 0);
}

// 加载证书链 + 私钥（可选 DH 参数文件），全部校验通过返回 true
// 参数：cert_file - 证书链 PEM 路径；key_file - 私钥 PEM 路径；dh_file - DH 参数 PEM 路径（可为空）
bool TlsContext::Load(const std::string& cert_file,
                      const std::string& key_file,
                      const std::string& dh_file)
{
    if (!ctx_) return false;

    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file.c_str()) != 1) {
        std::cerr << "[tls] 证书加载失败: " << cert_file << std::endl;
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        std::cerr << "[tls] 私钥加载失败: " << key_file << std::endl;
        return false;
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        std::cerr << "[tls] 私钥与证书不匹配: " << key_file << std::endl;
        return false;
    }
    if (!dh_file.empty() && !load_dh_file(ctx_, dh_file)) {
        std::cerr << "[tls] DH 参数加载失败: " << dh_file << std::endl;
        return false;
    }

    loaded_ = true;
    std::cout << "[tls] 已加载证书: " << cert_file << std::endl;
    return true;
}

}  // namespace net
