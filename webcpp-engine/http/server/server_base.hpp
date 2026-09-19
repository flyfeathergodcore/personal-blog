#pragma once
#include <memory>
#include <atomic>
#include <string>
#include "http/config/config.hpp"
#include "http/router/router.hpp"
#include "http/middleware/middleware.hpp"
#include "net/tls_context.h"

class ServerBase {
public:
    // 构造函数：保存配置、路由、中间件与 TLS 上下文，推导监听地址与端口
    // 参数：cfg - 服务配置；router - 路由表；middleware - 中间件管理器；tls - TLS 上下文
    ServerBase(const Config& cfg,
               Router& router,
               MiddlewareManager& middleware,
               std::shared_ptr<net::TlsContext> tls)
        : cfg_(cfg)
        , router_(router)
        , middleware_(middleware)
        , tls_(std::move(tls))
        , host_(cfg_.host)
        , port_(cfg_.tls_port > 0 ? cfg_.tls_port : cfg_.port)
    {}

    // 虚析构函数：默认实现，支持多态析构
    virtual ~ServerBase() = default;
    // 纯虚函数：启动服务（由子类实现）
    // 参数：无
    virtual void Start() = 0;

    /// Trigger graceful shutdown from a signal handler or another context.
    /// 置 shutdown_ 标志，通知各循环优雅退出
    void RequestShutdown() { shutdown_ = true; }
    // 查询服务是否已进入关闭流程
    // 参数：无；返回：是否已关闭
    bool IsShutdown() const { return shutdown_.load(); }

protected:
    const Config& cfg_;
    Router& router_;
    MiddlewareManager& middleware_;
    std::shared_ptr<net::TlsContext> tls_;
    std::string host_;
    uint16_t port_;
    std::atomic<bool> shutdown_{false};
    static constexpr int kDrainTimeoutSec = 30;
};
