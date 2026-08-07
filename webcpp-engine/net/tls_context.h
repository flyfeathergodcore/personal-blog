// net 层 TLS 上下文：裸 SSL_CTX*（无 asio），供 net::TlsStream 使用。
// 协程化改造的一部分：把 ssl/tls_context 的 asio::ssl::context 封装换成裸
// SSL_CTX*。本类型位于 net 命名空间，与原全局命名空间 TlsContext（asio 版）
// 共存，旧引擎在迁移期间仍编译链接旧类。
#pragma once

#include <string>

#include <openssl/ssl.h>

namespace net {

class TlsContext {
public:
    // 构造：创建服务端 SSL_CTX；失败时 NativeContext() 为 nullptr
    TlsContext();
    // 析构：释放 SSL_CTX
    ~TlsContext();

    // 禁止拷贝/移动
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&&) = delete;
    TlsContext& operator=(TlsContext&&) = delete;

    // 加载证书链 + 私钥（可选 DH 参数文件）。成功返回 true。
    bool Load(const std::string& cert_file, const std::string& key_file,
              const std::string& dh_file = {});
    // 返回底层 SSL_CTX*（未初始化/失败为 nullptr）
    SSL_CTX* NativeContext() { return ctx_; }
    const SSL_CTX* NativeContext() const { return ctx_; }
    // 是否已成功加载证书/私钥
    explicit operator bool() const { return loaded_; }

    /// 检查指定 SSL 会话是否经 ALPN 协商出 "h2"。
    static bool IsHttp2(SSL* ssl);

private:
    SSL_CTX* ctx_ = nullptr;
    bool loaded_ = false;
};

}  // namespace net
