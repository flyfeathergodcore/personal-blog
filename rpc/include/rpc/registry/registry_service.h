// 注册中心服务实现：内存存储 + 租约 TTL。
//
// 继承 protoc 插件生成的 RegistryServiceBase，实现 Register/Heartbeat/
// Deregister/Discover。单线程 EventLoop 假定（RpcServer 默认单线程驱动，
// 各 handler 串行执行，内部 map 无需加锁）。
//
// TTL 机制：
//   - Register 记录实例与首次心跳时间，返回唯一 instance_id；
//   - Heartbeat 刷新最后心跳时间戳（租约续期）；
//   - 过期判定：now - last_heartbeat > lease_seconds，由外部周期协程调用
//     SweepExpired() 剔除（server 每 ~1s 一次），Discover 查询时也会过滤
//     过期实例作为双保险。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "registry.pb.h"
#include "registry.rpc.h"

namespace rpc {

class RegistryService : public rpc::registry::RegistryServiceBase {
public:
    // Register：登记实例，分配唯一 instance_id（心跳/注销凭证）
    coro::Task<::rpc::registry::RegisterReply>
    Register(const ::rpc::registry::RegisterRequest& req) override;

    // Heartbeat：刷新实例最后心跳时间；未知 id 返回 ok=false
    coro::Task<::rpc::registry::HeartbeatReply>
    Heartbeat(const ::rpc::registry::HeartbeatRequest& req) override;

    // Deregister：主动注销；实例不存在返回 ok=false
    coro::Task<::rpc::registry::DeregisterReply>
    Deregister(const ::rpc::registry::DeregisterRequest& req) override;

    // Discover：返回指定服务名下未过期实例列表；空表示无可用实例
    coro::Task<::rpc::registry::DiscoverReply>
    Discover(const ::rpc::registry::DiscoverRequest& req) override;

    // 剔除所有过期实例，返回剔除数量（测试断言用）。now_ms 用 steady_clock
    std::size_t SweepExpired(int64_t now_ms);
    // 便捷重载：内部取当前时间（供周期清理协程调用）
    std::size_t SweepExpired();

    // 当前登记的实例总数（测试断言用）
    std::size_t size() const { return entries_.size(); }

private:
    // 单条实例记录
    struct Entry {
        ::rpc::registry::Instance inst;
        int64_t last_heartbeat_ms = 0;  // steady_clock 毫秒
    };

    // 判断实例是否已过期（Discover 过滤与 Sweep 共用）
    bool IsExpired(const Entry& e, int64_t now_ms) const;

    std::map<std::string, Entry> entries_;            // instance_id -> Entry
    std::map<std::string, std::vector<std::string>> by_service_;  // service -> ids
    std::uint64_t next_id_ = 1;                       // 分配 instance_id 用
};

}  // namespace rpc
