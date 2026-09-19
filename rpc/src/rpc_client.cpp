// RpcClient 门面实现：发现缓存 + 实例轮询 fallback + 一元重试（详见头文件）。
#include "rpc/rpc_client.h"

#include <chrono>

#include "coro/awaiter.h"
#include "rpc/error.h"

namespace rpc {

namespace {

// steady_clock 毫秒时间戳（与 registry TTL 一致的时间基准）
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 每个实例连接池的并发上限：单个实例同时最多借用该数连接
constexpr std::size_t kMaxPerEndpoint = 4;

}  // namespace

RpcClient::RpcClient(std::string registry_host, std::uint16_t registry_port,
                     std::int64_t discover_ttl_ms)
    : reg_host_(std::move(registry_host)), reg_port_(registry_port),
      discover_ttl_ms_(discover_ttl_ms) {}

coro::Task<std::shared_ptr<RpcChannel>> RpcClient::EnsureRegistry(int64_t timeout_ms) {
    // 连接已建且读循环存活：直接复用；否则（未建/断连）重建
    if (reg_ch_ && !reg_ch_->read_done()) co_return reg_ch_;
    reg_ch_ = std::make_shared<RpcChannel>();
    if (!co_await reg_ch_->Open(reg_host_, reg_port_, timeout_ms)) {
        reg_ch_.reset();
        co_return nullptr;
    }
    co_return reg_ch_;
}

coro::Task<std::vector<::rpc::registry::Instance>> RpcClient::Discover(
    std::string_view service, int64_t timeout_ms) {
    std::vector<::rpc::registry::Instance> out;
    auto rc = co_await EnsureRegistry(timeout_ms);
    if (!rc) co_return out;  // registry 不可达：视为无实例
    try {
        ::rpc::registry::RegistryClient reg(*rc);
        ::rpc::registry::DiscoverRequest dq;
        dq.set_service_name(std::string(service));
        ::rpc::registry::DiscoverReply rep = co_await reg.Discover(dq, timeout_ms);
        for (int i = 0; i < rep.instances_size(); ++i) out.push_back(rep.instances(i));
    } catch (const RpcException&) {
        // registry 调用了异常（断连/超时）：作废连接，下次自动重连
        if (reg_ch_) {
            reg_ch_->Close();
            reg_ch_.reset();
        }
    }
    co_return out;
}

coro::Task<void> RpcClient::RefreshInstances(std::string_view service, int64_t timeout_ms) {
    std::vector<::rpc::registry::Instance> insts = co_await Discover(service, timeout_ms);
    ServiceEntry e;
    e.instances = std::move(insts);
    e.refreshed_ms = NowMs();
    services_[std::string(service)] = std::move(e);
    co_return;
}

coro::Task<void> RpcClient::MaybeRefresh(std::string_view service, int64_t timeout_ms) {
    auto it = services_.find(std::string(service));
    if (it != services_.end() && NowMs() - it->second.refreshed_ms < discover_ttl_ms_) {
        co_return;  // 缓存未过期
    }
    co_await RefreshInstances(service, timeout_ms);
    co_return;
}

std::shared_ptr<RpcConnectionPool> RpcClient::PoolFor(const std::string& host,
                                                      std::uint16_t port) {
    const std::string key = host + ":" + std::to_string(port);
    auto it = pools_.find(key);
    if (it != pools_.end()) return it->second;
    auto pool = std::make_shared<RpcConnectionPool>(kMaxPerEndpoint);
    pools_[key] = pool;
    return pool;
}

coro::Task<std::shared_ptr<RpcChannel>> RpcClient::Acquire(std::string_view service,
                                                           int64_t timeout_ms) {
    const std::string key = std::string(service);
    co_await MaybeRefresh(service, timeout_ms);
    auto it = services_.find(key);
    if (it == services_.end() || it->second.instances.empty()) co_return nullptr;

    ServiceEntry& e = it->second;
    const std::size_t n = e.instances.size();
    // 从轮询游标起逐个实例尝试：连不上（Open 失败/池满超时）fallback 下一实例
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t idx = (e.rr_next + k) % n;
        const ::rpc::registry::Instance& inst = e.instances[idx];
        auto pool = PoolFor(inst.host(), inst.port());
        auto ch = co_await pool->Acquire(inst.host(), inst.port(), timeout_ms);
        if (ch) {
            e.rr_next = (idx + 1) % n;  // 下次从下一实例开始，实现轮询负载
            out_[ch.get()] = PoolRef{pool};
            co_return ch;
        }
    }
    co_return nullptr;  // 所有实例都连不上
}

void RpcClient::Release(const std::shared_ptr<RpcChannel>& conn, bool healthy) {
    auto it = out_.find(conn.get());
    if (it == out_.end()) return;  // 非本门面借出：忽略
    it->second.pool->Release(conn, healthy);
    out_.erase(it);
}

coro::Task<std::string> RpcClient::CallUnary(std::string_view service,
                                             std::string_view method,
                                             std::string_view payload,
                                             int64_t timeout_ms, int retries) {
    for (int i = 0; i <= retries; ++i) {
        auto ch = co_await Acquire(service, timeout_ms);
        if (!ch) {
            throw RpcException(RpcCode::NotFound,
                               "no available instance for " + std::string(service));
        }
        try {
            std::string resp = co_await ch->UnaryCall(std::string(service),
                                                      std::string(method),
                                                      std::string(payload), timeout_ms);
            Release(ch, true);
            co_return resp;
        } catch (const RpcException& e) {
            // co_await 不能出现在 catch handler 中（C++ 限制）：
            // 这里只标记连接失效并记录失败；重试逻辑在 try 块后统一处理
            Release(ch, false);  // 连接可能失效：池将关闭它
            if (i == retries) throw;  // 重试耗尽：rethrow 原异常（handler 内合法）
            (void)e;
        }
        // 非最后一次失败：重查实例，下轮 Acquire 换连接/换实例（try 块外，可 co_await）
        co_await RefreshInstances(service, timeout_ms);
    }
    co_return std::string();  // 不可达：retries >= 0，循环必执行一次
}

coro::Task<void> RpcClient::CloseAll() {
    for (auto& kv : pools_) co_await kv.second->CloseAll();
    pools_.clear();
    if (reg_ch_) {
        reg_ch_->Close();
        while (!reg_ch_->read_done()) co_await coro::sleep_for(10);
        reg_ch_.reset();
    }
    co_return;
}

}  // namespace rpc
