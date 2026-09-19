// 注册中心服务实现：内存存储 + 租约 TTL（详见头文件）。
#include "rpc/registry/registry_service.h"

#include <chrono>
#include <sstream>

namespace rpc {

namespace {

// steady_clock 毫秒时间戳：供心跳刷新、过期判定、Sweep 共用同一时钟
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 构造 instance_id："inst-" + 递增序号
std::string MakeInstanceId(std::uint64_t n) {
    std::ostringstream os;
    os << "inst-" << n;
    return os.str();
}

}  // namespace

bool RegistryService::IsExpired(const Entry& e, int64_t now_ms) const {
    const int64_t lease_ms = e.inst.lease_seconds() * 1000;
    return now_ms - e.last_heartbeat_ms > lease_ms;
}

coro::Task<::rpc::registry::RegisterReply>
RegistryService::Register(const ::rpc::registry::RegisterRequest& req) {
    ::rpc::registry::RegisterReply rep;
    const ::rpc::registry::Instance& inst = req.instance();
    if (inst.service_name().empty() || inst.host().empty() || inst.port() == 0) {
        // 参数不合法：instance_id 置空表示失败，由调用方识别
        co_return rep;
    }

    const std::string id = MakeInstanceId(next_id_++);
    Entry e;
    e.inst = inst;
    if (e.inst.lease_seconds() <= 0) e.inst.set_lease_seconds(60);  // 默认租约 60s
    e.last_heartbeat_ms = NowMs();

    entries_[id] = std::move(e);
    by_service_[inst.service_name()].push_back(id);
    rep.set_instance_id(id);
    co_return rep;
}

coro::Task<::rpc::registry::HeartbeatReply>
RegistryService::Heartbeat(const ::rpc::registry::HeartbeatRequest& req) {
    ::rpc::registry::HeartbeatReply rep;
    auto it = entries_.find(req.instance_id());
    if (it == entries_.end()) {
        rep.set_ok(false);  // 未知实例
        co_return rep;
    }
    it->second.last_heartbeat_ms = NowMs();  // 租约续期
    rep.set_ok(true);
    co_return rep;
}

coro::Task<::rpc::registry::DeregisterReply>
RegistryService::Deregister(const ::rpc::registry::DeregisterRequest& req) {
    ::rpc::registry::DeregisterReply rep;
    auto it = entries_.find(req.instance_id());
    if (it == entries_.end()) {
        rep.set_ok(false);
        co_return rep;
    }
    // 从 service 索引中移除该 id
    auto& ids = by_service_[it->second.inst.service_name()];
    for (auto b = ids.begin(), e = ids.end(); b != e; ++b) {
        if (*b == req.instance_id()) {
            ids.erase(b);
            break;
        }
    }
    entries_.erase(it);
    rep.set_ok(true);
    co_return rep;
}

coro::Task<::rpc::registry::DiscoverReply>
RegistryService::Discover(const ::rpc::registry::DiscoverRequest& req) {
    ::rpc::registry::DiscoverReply rep;
    const int64_t now_ms = NowMs();
    auto it = by_service_.find(req.service_name());
    if (it == by_service_.end()) co_return rep;  // 无此服务：返回空列表

    for (const std::string& id : it->second) {
        auto e = entries_.find(id);
        if (e == entries_.end()) continue;  // 索引与实际表不一致（理论不发生）
        if (IsExpired(e->second, now_ms)) continue;  // 过期实例不返回（双保险）
        *rep.add_instances() = e->second.inst;
    }
    co_return rep;
}

std::size_t RegistryService::SweepExpired() {
    return SweepExpired(NowMs());
}

std::size_t RegistryService::SweepExpired(int64_t now_ms) {
    // 先收集过期 id 再统一移除，避免迭代中修改 map
    std::vector<std::string> expired;
    for (const auto& kv : entries_) {
        if (IsExpired(kv.second, now_ms)) expired.push_back(kv.first);
    }
    for (const std::string& id : expired) {
        auto& ids = by_service_[entries_[id].inst.service_name()];
        for (auto b = ids.begin(), e = ids.end(); b != e; ++b) {
            if (*b == id) {
                ids.erase(b);
                break;
            }
        }
        entries_.erase(id);
    }
    return expired.size();
}

}  // namespace rpc
