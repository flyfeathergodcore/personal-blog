#include "http/server/upstream_conn_pool.hpp"
#include <algorithm>
#include <iostream>

// 获取线程局部单例（每 worker 一份连接池）
// 参数：无；返回：本线程唯一的连接池实例
UpstreamConnPool& UpstreamConnPool::Instance()
{
    thread_local UpstreamConnPool pool;
    return pool;
}

// 析构函数：关闭并清理所有空闲连接
// 参数：无
UpstreamConnPool::~UpstreamConnPool()
{
    // 析构前关闭所有空闲 socket
    for (auto& e : entries_) {
        if (e.conn && e.conn->socket.is_open())
            e.conn->socket.close();
    }
    entries_.clear();
}

// 获取 (host, port) 对应的空闲连接；无匹配或已失效返回 nullptr（会先清理过期连接）
// 参数：host - 上游主机；port - 上游端口
std::unique_ptr<UpstreamConnPool::Conn>
UpstreamConnPool::Acquire(const std::string& host, unsigned short port)
{
    EvictStale();

    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (it->host == host && it->port == port && it->conn)
        {
            auto conn = std::move(it->conn);
            entries_.erase(it);

            // 校验 socket 是否仍可用（非侵入式检查）
            if (conn->socket.is_open())
                return conn;

            // 被上游关闭——丢弃并继续扫描
            // （EvictStale 已移除关闭的连接，正常情况下不会走到这里，防御性处理）
        }
    }

    return nullptr;
}

// 归还连接至空闲池供复用；池容量满时淘汰一条最久未用的连接
// 参数：conn - 待归还连接（池接管所有权，调用后勿再使用）；host - 上游主机；port - 上游端口
void UpstreamConnPool::Release(std::unique_ptr<Conn> conn,
                                const std::string& host,
                                unsigned short port)
{
    if (!conn || !conn->socket.is_open())
        return;

    conn->last_used = std::chrono::steady_clock::now();

    // 容量满时淘汰一条最久未用的
    if (entries_.size() >= kMaxEntries)
    {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin() + 1; it != entries_.end(); ++it)
        {
            if (it->conn && it->conn->last_used < oldest->conn->last_used)
                oldest = it;
        }
        entries_.erase(oldest);
    }

    Entry entry;
    entry.conn  = std::move(conn);
    entry.host  = host;
    entry.port  = port;
    entries_.push_back(std::move(entry));
}

// 返回当前空闲连接数
// 参数：无；返回：空闲数量
size_t UpstreamConnPool::IdleCount() const
{
    return entries_.size();
}

// 淘汰过期/失效的空闲连接（空闲超时或 socket 已关闭）
// 参数：无
void UpstreamConnPool::EvictStale()
{
    if (entries_.empty()) return;

    auto now = std::chrono::steady_clock::now();

    for (auto it = entries_.begin(); it != entries_.end(); )
    {
        bool remove = false;
        if (!it->conn)
        {
            remove = true;
        }
        else if (!it->conn->socket.is_open())
        {
            remove = true;
        }
        else if ((now - it->conn->last_used) > kIdleTimeout)
        {
            it->conn->socket.close();
            remove = true;
        }

        if (remove)
            it = entries_.erase(it);
        else
            ++it;
    }
}
