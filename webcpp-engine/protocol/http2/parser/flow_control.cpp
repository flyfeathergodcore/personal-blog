#include "protocol/http2/parser/flow_control.hpp"
#include <algorithm>

// ═══════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════

// 构造函数：以指定初始窗口初始化连接级信用，流级窗口延迟到首次接触时创建。
// 参数：initial_window - 初始窗口大小（字节，默认 65535）
H2FlowControl::H2FlowControl(uint32_t initial_window)
    : initial_window_size_(initial_window)
{
    conn_.credit = static_cast<int32_t>(initial_window);
    conn_.consumed = 0;
}

// 重新配置初始窗口大小（来自本端 SETTINGS），并把增量同步应用到连接窗口及所有已存在流的窗口。
// 参数：size - 新的初始窗口大小
void H2FlowControl::SetInitialWindow(uint32_t size)
{
    int32_t delta = static_cast<int32_t>(size) - static_cast<int32_t>(initial_window_size_);
    initial_window_size_ = size;

    // Adjust connection window
    conn_.credit += delta;

    // Adjust all existing stream windows
    for (auto& [id, ws] : streams_)
        ws.credit += delta;
}

// 处理对端 SETTINGS 的 INITIAL_WINDOW_SIZE：对本端而言与 SetInitialWindow 等价，
// 按增量调整连接与已有各流的窗口。
// 参数：size - 对端设置的初始窗口大小
void H2FlowControl::SetPeerInitialWindow(uint32_t size)
{
    // Actually, this is the same as SetInitialWindow from our perspective.
    // When the peer sends SETTINGS with INITIAL_WINDOW_SIZE, it affects
    // streams that haven't been opened yet (or all, with the delta adjustment).
    SetInitialWindow(size);
}

// ═══════════════════════════════════════════════════════════════
// Window state access
// ═══════════════════════════════════════════════════════════════

// 获取指定流的窗口状态，不存在则按初始窗口创建；流 ID 0 表示连接级窗口。
// 参数：stream_id - 流 ID（0 = 连接级）
H2FlowControl::WindowState& H2FlowControl::GetOrCreate(uint32_t stream_id)
{
    if (stream_id == 0)
        return conn_;

    auto it = streams_.find(stream_id);
    if (it != streams_.end())
        return it->second;

    // First time we hear about this stream — initialise
    auto& ws = streams_[stream_id];
    ws.credit = static_cast<int32_t>(initial_window_size_);
    ws.consumed = 0;
    return ws;
}

// ═══════════════════════════════════════════════════════════════
// Consume + credit tracking
// ═══════════════════════════════════════════════════════════════

// 记录已接收并消费的 n 字节：扣减流与连接级信用、累加已消费计数。
// 参数：stream_id - 流 ID；n - 消费的字节数
void H2FlowControl::ConsumeBytes(uint32_t stream_id, uint32_t n)
{
    auto& ws = GetOrCreate(stream_id);
    ws.credit -= static_cast<int32_t>(n);
    ws.consumed += n;

    // Also consume from connection window
    if (stream_id != 0) {
        conn_.credit -= static_cast<int32_t>(n);
        conn_.consumed += n;
    }
}

// 判断是否该为该实体发送 WINDOW_UPDATE：已消费字节达到初始窗口一半时返回 true。
// 参数：stream_id - 流 ID（0 = 连接级）
bool H2FlowControl::ShouldUpdate(uint32_t stream_id) const
{
    if (stream_id == 0)
        return conn_.consumed >= initial_window_size_ / 2;

    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return false;
    return it->second.consumed >= initial_window_size_ / 2;
}

// 取出应写入 WINDOW_UPDATE 帧的信用值，并清零已消费计数、恢复对应信用。
// 参数：stream_id - 流 ID（0 = 连接级）；返回窗口更新增量
uint32_t H2FlowControl::PopCredit(uint32_t stream_id)
{
    auto& ws = GetOrCreate(stream_id);
    uint32_t credit = ws.consumed;
    ws.consumed = 0;
    ws.credit += static_cast<int32_t>(credit);
    return credit;
}

// 查询当前可用信用（调试用）；流级窗口取流与连接窗口的较小值，未知流返回初始窗口。
// 参数：stream_id - 流 ID（0 = 连接级）
uint32_t H2FlowControl::Available(uint32_t stream_id) const
{
    if (stream_id == 0) {
        uint32_t c = conn_.credit > 0
            ? static_cast<uint32_t>(conn_.credit) : 0;
        // Take min with stream's actual credit
        return c;
    }

    auto it = streams_.find(stream_id);
    if (it == streams_.end())
        return initial_window_size_;

    // Effective window = min(stream, connection)
    uint32_t stream_c = it->second.credit > 0
        ? static_cast<uint32_t>(it->second.credit) : 0;
    uint32_t conn_c = conn_.credit > 0
        ? static_cast<uint32_t>(conn_.credit) : 0;

    return std::min(stream_c, conn_c);
}
