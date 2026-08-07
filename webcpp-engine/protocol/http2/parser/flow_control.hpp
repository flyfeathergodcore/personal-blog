#pragma once
#include <cstdint>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════
// H2FlowControl — connection-level + per-stream window management
//
// Each stream (and the connection itself) starts with an initial
// window of 65535 bytes.  The peer cannot send more data than
// the available credit.  When the application has consumed the
// data, it sends a WINDOW_UPDATE frame to restore credit.
//
// Strategy (simple, reasonable for MVP):
//   ConsumedBytes() accumulates.  When consumed >= initial/2,
//   ShouldUpdate() returns true, and Credit() tells you how
//   much to write in the WINDOW_UPDATE frame.
// ═══════════════════════════════════════════════════════════════

class H2FlowControl {
public:
    // 构造函数：初始化连接级窗口。
    // 参数：initial_window - 初始窗口大小（字节，默认 65535）
    explicit H2FlowControl(uint32_t initial_window = 65535);

    /// 重新配置初始窗口大小（来自 SETTINGS）。
    void SetInitialWindow(uint32_t size);

    /// 记录已接收并消费 n 字节。
    void ConsumeBytes(uint32_t stream_id, uint32_t n);

    /// 是否该为该实体发送 WINDOW_UPDATE。
    bool ShouldUpdate(uint32_t stream_id) const;

    /// 应写入 WINDOW_UPDATE 帧的字节数；调用会清零已消费计数。
    uint32_t PopCredit(uint32_t stream_id);

    /// 当前可用信用（调试用）。
    uint32_t Available(uint32_t stream_id) const;

    /// 设置对端 SETTINGS 中的 INITIAL_WINDOW_SIZE。
    void SetPeerInitialWindow(uint32_t size);

private:
    struct WindowState {
        int32_t credit;     // available credit (can go negative with SETTINGS changes)
        uint32_t consumed;  // bytes consumed since last WINDOW_UPDATE
    };

    uint32_t initial_window_size_ = 65535;
    WindowState conn_;   // stream_id = 0
    std::unordered_map<uint32_t, WindowState> streams_;

    // 获取流窗口状态，不存在则按初始窗口创建（流 ID 0 = 连接级）。
    WindowState& GetOrCreate(uint32_t stream_id);
};
