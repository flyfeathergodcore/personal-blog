#include "protocol/http2/parser/flow_control.hpp"
#include <algorithm>

// ═══════════════════════════════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════════════════════════════

// 构造函数：初始化两本【连接级】账；流级账延迟到首次接触时创建。
// 参数：initial_window - 初始窗口大小（字节，默认 65535）
H2FlowControl::H2FlowControl(uint32_t initial_window)
    : recv_initial_(initial_window)
    , send_initial_(initial_window)
{
    conn_recv_.credit = static_cast<int32_t>(initial_window);
    conn_recv_.consumed = 0;
    conn_send_ = static_cast<int32_t>(initial_window);
}

// ═══════════════════════════════════════════════════════════════
// 内部状态访问
// ═══════════════════════════════════════════════════════════════

// 取流级接收账，不存在则按 recv_initial_ 创建；流 ID 0 返回连接账。
// 参数：stream_id - 流 ID（0 = 连接级）
H2FlowControl::RecvState& H2FlowControl::GetOrCreateRecv(uint32_t stream_id)
{
    if (stream_id == 0)
        return conn_recv_;

    auto it = recv_.find(stream_id);
    if (it != recv_.end())
        return it->second;

    auto& st = recv_[stream_id];
    st.credit = static_cast<int32_t>(recv_initial_);
    st.consumed = 0;
    return st;
}

// 取流级发送账，不存在则按 send_initial_ 创建。
// 参数：stream_id - 流 ID（调用方需保证非 0）
int32_t& H2FlowControl::GetOrCreateSend(uint32_t stream_id)
{
    auto it = send_.find(stream_id);
    if (it != send_.end())
        return it->second;

    return send_.emplace(stream_id, static_cast<int32_t>(send_initial_)).first->second;
}

// 把 int32_t 窗口转成对外呈现的 uint32_t。负窗口是合法的中间状态
// （SETTINGS 调小所致，§6.9.2），但对调用方而言就是"现在一个字节也发不了"。
// 参数：v - 原始窗口值
uint32_t H2FlowControl::Clamp(int32_t v)
{
    return v > 0 ? static_cast<uint32_t>(v) : 0;
}

// ═══════════════════════════════════════════════════════════════
// 接收侧
// ═══════════════════════════════════════════════════════════════

// 记录已接收并消费的 n 字节：两本接收账同时扣，并累加已消费计数。
// 【契约】连接账在本方法内部扣，调用方不要再单独调 ConsumeRecv(0, n)。
// 参数：stream_id - 流 ID；n - 消费的字节数
void H2FlowControl::ConsumeRecv(uint32_t stream_id, uint32_t n)
{
    auto& ws = GetOrCreateRecv(stream_id);
    ws.credit -= static_cast<int32_t>(n);
    ws.consumed += n;

    // 连接账在同一处扣 —— 流 ID 0 时上面的 ws 就是连接账，不重复扣
    if (stream_id != 0) {
        conn_recv_.credit -= static_cast<int32_t>(n);
        conn_recv_.consumed += n;
    }
}

// 查询当前可接收额度：只报该实体自己的接收窗口，负值 clamp 到 0。
// 流级不做与连接窗口的 min —— 那是调用方按需组合两个 RecvWindow 的事。
// 参数：stream_id - 流 ID（0 = 连接级）
uint32_t H2FlowControl::RecvWindow(uint32_t stream_id) const
{
    if (stream_id == 0)
        return Clamp(conn_recv_.credit);

    auto it = recv_.find(stream_id);
    if (it == recv_.end())
        return recv_initial_;   // 未知流：按初始窗口满额

    return Clamp(it->second.credit);
}

// 判断是否该为该实体发 WINDOW_UPDATE：已消费达到初始窗口一半时返回 true。
// 参数：stream_id - 流 ID（0 = 连接级）
bool H2FlowControl::ShouldUpdate(uint32_t stream_id) const
{
    if (stream_id == 0)
        return conn_recv_.consumed >= recv_initial_ / 2;

    auto it = recv_.find(stream_id);
    if (it == recv_.end()) return false;
    return it->second.consumed >= recv_initial_ / 2;
}

// 取出发 WINDOW_UPDATE 的增量：清零已消费计数，并把等量额度还回接收账。
// 还回的时机与"帧真的写进输出缓冲"一致，因此本端账目与对端即将看到的一致。
// 参数：stream_id - 流 ID（0 = 连接级）；返回：WINDOW_UPDATE 帧的增量
uint32_t H2FlowControl::PopCredit(uint32_t stream_id)
{
    if (stream_id == 0) {
        uint32_t credit = conn_recv_.consumed;
        conn_recv_.consumed = 0;
        conn_recv_.credit += static_cast<int32_t>(credit);
        return credit;
    }

    auto& ws = GetOrCreateRecv(stream_id);
    uint32_t credit = ws.consumed;
    ws.consumed = 0;
    ws.credit += static_cast<int32_t>(credit);
    return credit;
}

// ═══════════════════════════════════════════════════════════════
// 发送侧
// ═══════════════════════════════════════════════════════════════

// 查询当前可发送额度：流级窗口取流与连接的较小值。
// 参数：stream_id - 流 ID（0 = 连接级）
uint32_t H2FlowControl::SendWindow(uint32_t stream_id) const
{
    if (stream_id == 0)
        return Clamp(conn_send_);

    auto it = send_.find(stream_id);
    if (it == send_.end())
        return std::min(Clamp(static_cast<int32_t>(send_initial_)), Clamp(conn_send_));

    return std::min(Clamp(it->second), Clamp(conn_send_));
}

// 记录本端发出了 n 字节 DATA payload：两本发送账同时扣。
// 参数：stream_id - 流 ID；n - 发出的 payload 字节数
void H2FlowControl::ConsumeSend(uint32_t stream_id, uint32_t n)
{
    if (stream_id == 0) {
        conn_send_ -= static_cast<int32_t>(n);
        return;
    }

    GetOrCreateSend(stream_id) -= static_cast<int32_t>(n);
    conn_send_ -= static_cast<int32_t>(n);
}

// 收到对端 WINDOW_UPDATE：为该实体补充发送额度。
// 参数：stream_id - 流 ID（0 = 连接级）；n - 增量
void H2FlowControl::AddSendCredit(uint32_t stream_id, uint32_t n)
{
    if (stream_id == 0) {
        conn_send_ += static_cast<int32_t>(n);
        return;
    }

    // 即便该流已关闭，也照记不误：RFC 7540 §5.1 规定流关闭后仍可能收到
    // WINDOW_UPDATE，忽略它不会出错，而创建一条孤立的账目代价极低。
    GetOrCreateSend(stream_id) += static_cast<int32_t>(n);
}

// ═══════════════════════════════════════════════════════════════
// SETTINGS
// ═══════════════════════════════════════════════════════════════

// 应用【本端】SETTINGS 的 INITIAL_WINDOW_SIZE：按增量调整流级接收窗口。
// 不触碰连接级接收窗口 —— RFC 7540 §6.9.2 明确规定该设置只影响流级。
// 参数：size - 新的初始窗口大小
void H2FlowControl::SetLocalInitialWindow(uint32_t size)
{
    int32_t delta = static_cast<int32_t>(size) - static_cast<int32_t>(recv_initial_);
    recv_initial_ = size;

    for (auto& [id, st] : recv_)
        st.credit += delta;
}

// 应用【对端】SETTINGS 的 INITIAL_WINDOW_SIZE：按增量调整流级发送窗口。
// 不触碰连接级发送窗口（§6.9.2），也不触碰任何接收账（§6.5.2）。
// 参数：size - 对端设置的初始窗口大小
void H2FlowControl::SetPeerInitialWindow(uint32_t size)
{
    int32_t delta = static_cast<int32_t>(size) - static_cast<int32_t>(send_initial_);
    send_initial_ = size;

    // 允许结果为负：§6.9.2 要求这种情况下发送方暂停，
    // 直到对端用 WINDOW_UPDATE 把窗口加回正数。
    for (auto& [id, credit] : send_)
        credit += delta;
}

// ═══════════════════════════════════════════════════════════════
// 生命周期
// ═══════════════════════════════════════════════════════════════

// 流关闭时移除它的两本账。
// 参数：stream_id - 流 ID（0 是连接级，本方法忽略）
void H2FlowControl::RemoveStream(uint32_t stream_id)
{
    if (stream_id == 0) return;
    recv_.erase(stream_id);
    send_.erase(stream_id);
}

// 该流的账目是否还在。
// 参数：stream_id - 流 ID
bool H2FlowControl::HasStream(uint32_t stream_id) const
{
    return recv_.count(stream_id) > 0 || send_.count(stream_id) > 0;
}
