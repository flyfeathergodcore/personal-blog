#include "server/upstream_conn_pool.hpp"
#include <algorithm>
#include <iostream>

UpstreamConnPool& UpstreamConnPool::Instance()
{
    thread_local UpstreamConnPool pool;
    return pool;
}

UpstreamConnPool::~UpstreamConnPool()
{
    // 析构前关闭所有空闲 socket
    for (auto& e : entries_) {
        if (e.conn && e.conn->socket.is_open())
            e.conn->socket.close();
    }
    entries_.clear();
}

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

size_t UpstreamConnPool::IdleCount() const
{
    return entries_.size();
}

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
