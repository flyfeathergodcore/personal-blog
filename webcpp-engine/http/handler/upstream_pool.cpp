#include "http/handler/upstream_pool.hpp"
#include <algorithm>
#include <iostream>

// 构造：保存上游服务器列表（空列表时输出警告）
// 参数：servers - 上游服务器列表
UpstreamPool::UpstreamPool(std::vector<UpstreamServer> servers)
    : servers_(std::move(servers))
{
    if (servers_.empty())
        std::cerr << "[upstream] 警告：无上游服务器" << std::endl;
}

// 轮询选取一个健康的上游；对冷却期已过的死节点自动恢复，全死则返回 nullptr
const UpstreamServer* UpstreamPool::Pick()
{
    if (servers_.empty()) return nullptr;

    auto now = std::chrono::steady_clock::now();

    // Try up to servers_.size() times to find a healthy server.
    for (size_t attempt = 0; attempt < servers_.size(); attempt++)
    {
        auto idx = counter_.fetch_add(1, std::memory_order_relaxed) % servers_.size();
        auto& srv = servers_[idx];

        if (srv.alive) {
            return &srv;
        }

        // Dead — check cooldown expiry
        if (!srv.alive) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - srv.dead_since).count();
            if (elapsed >= UpstreamServer::kCooldownMs) {
                srv.alive = true;
                srv.failures = 0;
                std::cout << "[upstream] 恢复 " << srv.host << ":"
                          << srv.port << std::endl;
                return &srv;
            }
        }
    }

    return nullptr;  // all dead
}

// 上报一次失败：连续失败达到阈值时摘除该上游（标记死亡并记录时间）
// 参数：server - 发生失败的上游（由 Pick 返回的指针）
void UpstreamPool::ReportFailure(const UpstreamServer* server)
{
    if (!server) return;

    for (auto& srv : servers_)
    {
        if (&srv == server)
        {
            srv.failures++;
            if (srv.failures >= kMaxFailures && srv.alive)
            {
                srv.alive = false;
                srv.dead_since = std::chrono::steady_clock::now();
                std::cout << "[upstream] 摘除 " << srv.host << ":"
                          << srv.port << " (连续 " << srv.failures
                          << " 次失败)" << std::endl;
            }
            return;
        }
    }
}

// 上报一次成功：清零失败计数，若该上游处于死亡状态则恢复为健康
// 参数：server - 请求成功的上游（由 Pick 返回的指针）
void UpstreamPool::ReportSuccess(const UpstreamServer* server)
{
    if (!server) return;

    for (auto& srv : servers_)
    {
        if (&srv == server && srv.failures > 0)
        {
            srv.failures = 0;
            if (!srv.alive) {
                srv.alive = true;
                std::cout << "[upstream] 恢复 " << srv.host << ":"
                          << srv.port << std::endl;
            }
            return;
        }
    }
}
