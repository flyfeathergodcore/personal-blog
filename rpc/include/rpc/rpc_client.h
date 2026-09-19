// RpcClient：业务客户端统一门面（面向消费者的高层 API）。
//
// 职责：
//   1. Discover     —— 向 registry 查询 service 实例列表（带缓存 TTL）；
//   2. Acquire      —— 为 service 取一个可用连接：per-实例连接池 + 轮询负载，
//                      实例不可用（连不上/已断连）自动 fallback 下一个实例；
//   3. CallUnary    —— 一元调用门面：失败（连接断开/超时/实例挂了）自动刷新实例
//                      并换连接重试，最多 retries 次；
//   4. Release      —— 归还连接；healthy=false 时通知池关闭失效连接；
//   5. CloseAll     —— 关闭全部连接池与 registry 连接。
//
// 连接所有权约定：Acquire 得到的连接必须用 Release 归还（每借必还）。
// 单线程 EventLoop 假定（与框架其余部分一致）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "coro/task.h"
#include "rpc/rpc_channel.h"
#include "rpc/rpc_pool.h"
#include "registry.pb.h"
#include "registry.rpc.h"

namespace rpc {

class RpcClient {
public:
    // 参数：registry_host / registry_port - 注册中心地址（必填）
    //       discover_ttl_ms - 发现结果缓存有效期，过期后 Acquire 自动重查
    RpcClient(std::string registry_host, std::uint16_t registry_port,
              std::int64_t discover_ttl_ms = 5000);

    // 查询 service 全部有效实例（每次直查 registry，不走缓存）
    coro::Task<std::vector<::rpc::registry::Instance>> Discover(
        std::string_view service, std::int64_t timeout_ms = 3000);

    // 取一个可用连接（轮询实例 + 实例间 fallback）；无可用实例返回 nullptr。
    // 返回的连接必须在用完后调用 Release 归还。
    coro::Task<std::shared_ptr<RpcChannel>> Acquire(std::string_view service,
                                                    std::int64_t timeout_ms = 3000);

    // 归还连接；healthy=false 表示连接已失效（调用失败），池会关闭它
    void Release(const std::shared_ptr<RpcChannel>& conn, bool healthy = true);

    // 一元调用门面：自动发现 + 失败重试 + 实例 fallback。
    // 参数：service/method - 调用目标；payload - 请求体 bytes；
    //       timeout_ms - 单次调用超时；retries - 失败后额外重试次数
    // 抛 RpcException：无可用实例(NotFound) / 重试耗尽
    coro::Task<std::string> CallUnary(std::string_view service, std::string_view method,
                                      std::string_view payload,
                                      std::int64_t timeout_ms = 3000,
                                      int retries = 2);

    // 关闭全部连接池与 registry 连接
    coro::Task<void> CloseAll();

private:
    struct PoolRef {  // Acquire 借出记录：连接 → 所属池
        std::shared_ptr<RpcConnectionPool> pool;
    };

    // 确保 registry 连接可用（懒建；断连自动重建）
    coro::Task<std::shared_ptr<RpcChannel>> EnsureRegistry(std::int64_t timeout_ms);
    // 若发现缓存过期则重新 Discover 更新实例列表
    coro::Task<void> MaybeRefresh(std::string_view service, std::int64_t timeout_ms);
    // 强制刷新某 service 的实例缓存
    coro::Task<void> RefreshInstances(std::string_view service, std::int64_t timeout_ms);
    // 按 "host:port" 取（或建）实例连接池
    std::shared_ptr<RpcConnectionPool> PoolFor(const std::string& host,
                                               std::uint16_t port);

    std::string reg_host_;
    std::uint16_t reg_port_;
    std::int64_t discover_ttl_ms_;

    // registry 连接（懒建；多个 Discover 复用同一连接）
    std::shared_ptr<RpcChannel> reg_ch_;

    // 每个 service 的发现缓存与轮询游标
    struct ServiceEntry {
        std::vector<::rpc::registry::Instance> instances;
        std::int64_t refreshed_ms = 0;  // 缓存时间戳
        std::size_t rr_next = 0;        // 轮询游标
    };
    std::map<std::string, ServiceEntry> services_;

    // per 实例连接池（key: "host:port"）
    std::map<std::string, std::shared_ptr<RpcConnectionPool>> pools_;
    // 借出记录：连接 → 所属池（Release 时据此归还）
    std::map<const void*, PoolRef> out_;
};

}  // namespace rpc
