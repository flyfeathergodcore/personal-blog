#pragma once
#include <memory>
#include <atomic>
#include <string>
#include "config/config.hpp"
#include "router/router.hpp"
#include "middleware/middleware.hpp"
#include "net/tls_context.h"

class ServerBase {
public:
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

    virtual ~ServerBase() = default;
    virtual void Start() = 0;

    /// Trigger graceful shutdown from a signal handler or another context.
    void RequestShutdown() { shutdown_ = true; }
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
