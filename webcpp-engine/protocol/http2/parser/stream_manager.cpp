#include "protocol/http2/parser/stream_manager.hpp"

// ═══════════════════════════════════════════════════════════════
// Stream lifecycle
// ═══════════════════════════════════════════════════════════════

// 处理客户端发起的 HEADERS：校验流 ID 必须为奇数、单调递增且未超过并发上限。
// 参数：stream_id - 新流的 ID；返回 true 表示接受，false 表示协议错误
H2StreamManager::OpenResult H2StreamManager::OnStreamOpen(int32_t stream_id)
{
    // Client-initiated streams MUST have odd IDs
    if ((stream_id & 1) == 0)
        return OpenResult::ProtocolError;

    // Stream IDs MUST monotonically increase (no reuse)
    if (stream_id <= last_client_stream_id_)
        return OpenResult::ProtocolError;

    // Must not exceed max concurrent streams
    if (active_count_ >= max_concurrent_)
        return OpenResult::Refused;

    last_client_stream_id_ = stream_id;
    states_[stream_id] = H2StreamState::Open;
    active_count_++;
    return OpenResult::Accepted;
}

// 处理客户端 END_STREAM：将 open 状态流转为 half_closed_remote。
// 参数：stream_id - 流 ID
void H2StreamManager::OnStreamEndStream(int32_t stream_id)
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return;

    // open → half_closed_remote
    if (it->second == H2StreamState::Open)
        it->second = H2StreamState::HalfClosedRemote;
}

// 处理 RST_STREAM 或本端发起的关闭：将流标记为 closed 并减少活动计数。
// 参数：stream_id - 流 ID
void H2StreamManager::OnStreamClose(int32_t stream_id)
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return;

    if (it->second != H2StreamState::Closed) {
        it->second = H2StreamState::Closed;
        if (active_count_ > 0) active_count_--;
    }
}

// 移除一个流：若尚未 closed 先执行 OnStreamClose，再从状态表删除。
// 参数：stream_id - 流 ID
void H2StreamManager::RemoveStream(int32_t stream_id)
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return;

    if (it->second != H2StreamState::Closed)
        OnStreamClose(stream_id);

    states_.erase(it);
}

// ═══════════════════════════════════════════════════════════════
// State queries
// ═══════════════════════════════════════════════════════════════

// 查询指定流的当前状态，未知流返回 Idle。
// 参数：stream_id - 流 ID
H2StreamState H2StreamManager::GetState(int32_t stream_id) const
{
    auto it = states_.find(stream_id);
    return it != states_.end() ? it->second : H2StreamState::Idle;
}

// 判断流是否处于活动状态（open 或 half-closed）。
// 参数：stream_id - 流 ID
bool H2StreamManager::IsActive(int32_t stream_id) const
{
    auto it = states_.find(stream_id);
    if (it == states_.end()) return false;

    return it->second == H2StreamState::Open
        || it->second == H2StreamState::HalfClosedRemote
        || it->second == H2StreamState::HalfClosedLocal;
}

// ═══════════════════════════════════════════════════════════════
// Pending queue
// ═══════════════════════════════════════════════════════════════

// 将已完成的请求流加入顺序处理队列。
// 参数：stream_id - 流 ID
void H2StreamManager::Enqueue(int32_t stream_id)
{
    pending_.push_back(stream_id);
}

// 弹出下一个待处理流，队列为空返回 0。
int32_t H2StreamManager::Dequeue()
{
    if (pending_.empty()) return 0;
    int32_t sid = pending_.front();
    pending_.pop_front();
    return sid;
}

// ═══════════════════════════════════════════════════════════════
// Limits
// ═══════════════════════════════════════════════════════════════

// 判断当前活动流数是否达到并发上限，未达上限即可新建流。
bool H2StreamManager::CanCreateStream() const
{
    return active_count_ < max_concurrent_;
}

// ═══════════════════════════════════════════════════════════════
// Garbage collection
// ═══════════════════════════════════════════════════════════════

// 清理状态表中所有 closed 状态的流（垃圾回收）。
void H2StreamManager::GcClosed()
{
    for (auto it = states_.begin(); it != states_.end(); ) {
        if (it->second == H2StreamState::Closed) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}
