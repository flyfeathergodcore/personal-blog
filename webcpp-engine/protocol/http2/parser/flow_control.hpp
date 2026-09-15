#pragma once
#include <cstdint>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════
// H2FlowControl — 双向流控账本
//
// 本类只做账：不做 IO、不认识协程、不持有任何事件循环概念。四组状态：
//
//   接收账 —— 对端还能发给我们多少
//     conn_recv_ —— 连接级，初始 = 构造函数给的初始窗口（默认 65535）；
//                   此后【不再受 SETTINGS 影响】，只能由我们发 WINDOW_UPDATE 补充
//     recv_[sid] —— 流级，初始 = 本端 SETTINGS 的 INITIAL_WINDOW_SIZE
//
//   发送账 —— 我们还能发给对端多少
//     conn_send_ —— 连接级，初始 = 构造函数给的初始窗口（默认 65535）；
//                   此后【不再受 SETTINGS 影响】，只能由对端 WINDOW_UPDATE 补充
//     send_[sid] —— 流级，初始 = 对端 SETTINGS 的 INITIAL_WINDOW_SIZE
//
// 两条 RFC 规则划定了这四组状态之间的边界（旧实现两条都违反了）：
//
//   §6.9.2  SETTINGS_INITIAL_WINDOW_SIZE 只改变【流级】窗口。连接级窗口
//           不受它影响，只能由 WINDOW_UPDATE 改变。
//
//   §6.5.2  对端 SETTINGS 里的 INITIAL_WINDOW_SIZE 作用于【我们的发送账】；
//           本端 SETTINGS 里的作用于【我们的接收账】。两者不可互相调用——
//           这正是旧 SetPeerInitialWindow 转手调 SetInitialWindow 的错处。
//
// 窗口值用 int32_t：SETTINGS 调小初始窗口时，已有流的窗口可以被压成负数
// （§6.9.2 明确允许，对端后续的 WINDOW_UPDATE 会把它加回来）。因此内部
// 原样保留负值，只在对外读取（RecvWindow / SendWindow）时 clamp 到 0。
// ═══════════════════════════════════════════════════════════════

class H2FlowControl {
public:
    // 构造函数：以指定的初始窗口初始化两本连接级账；流级账延迟到首次接触时创建。
    // 参数：initial_window - 初始窗口大小（字节，默认 65535）
    explicit H2FlowControl(uint32_t initial_window = 65535);

    // ── 接收侧：对端发来的数据 ──

    /// 记录本端收到并消费 n 字节。两本接收账同时扣。
    /// 【契约】调用方只调一次——连接账在本方法内部扣，不要再单独调 ConsumeRecv(0, n)。
    /// 参数：stream_id - 流 ID（0 = 连接级）；n - 消费的字节数
    void ConsumeRecv(uint32_t stream_id, uint32_t n);

    /// 本端还能接收多少字节。流级取流与连接的较小值，负值 clamp 到 0。
    /// 参数：stream_id - 流 ID（0 = 连接级）；返回：可用接收额度
    uint32_t RecvWindow(uint32_t stream_id) const;

    /// 是否该为该实体发送 WINDOW_UPDATE（已消费 ≥ 初始窗口一半）。
    /// 参数：stream_id - 流 ID（0 = 连接级）
    bool ShouldUpdate(uint32_t stream_id) const;

    /// 取出发送 WINDOW_UPDATE 的增量并清零已消费计数，同时把额度还回接收账。
    /// 参数：stream_id - 流 ID（0 = 连接级）；返回：WINDOW_UPDATE 帧应携带的增量
    uint32_t PopCredit(uint32_t stream_id);

    // ── 发送侧：本端发出去的数据 ──

    /// 本端还能发送多少字节。流级取流与连接的较小值，负值 clamp 到 0。
    /// 参数：stream_id - 流 ID（0 = 连接级）；返回：可用发送额度
    uint32_t SendWindow(uint32_t stream_id) const;

    /// 记录本端发出了 n 字节 DATA payload。两本发送账同时扣。
    /// 注意：n 是 DATA 帧 payload 长度，不含 9 字节帧头（RFC 7540 §6.9.1）。
    /// 参数：stream_id - 流 ID；n - 发出的 payload 字节数
    void ConsumeSend(uint32_t stream_id, uint32_t n);

    /// 收到对端 WINDOW_UPDATE：为该实体补充 n 字节发送额度。
    /// 参数：stream_id - 流 ID（0 = 连接级）；n - 增量
    void AddSendCredit(uint32_t stream_id, uint32_t n);

    // ── SETTINGS ──

    /// 应用【本端】SETTINGS 的 INITIAL_WINDOW_SIZE：按增量调整流级接收窗口。
    /// 不触碰连接级接收窗口（§6.9.2）。
    /// 参数：size - 新的初始窗口大小
    void SetLocalInitialWindow(uint32_t size);

    /// 应用【对端】SETTINGS 的 INITIAL_WINDOW_SIZE：按增量调整流级发送窗口。
    /// 不触碰连接级发送窗口（§6.9.2），也不触碰任何接收账（§6.5.2）。
    /// 参数：size - 对端设置的初始窗口大小
    void SetPeerInitialWindow(uint32_t size);

    // ── 生命周期 ──

    /// 流关闭时移除它的两本账，避免映射随连接生命周期无界增长。
    /// 参数：stream_id - 流 ID（0 是连接级，本方法忽略）
    void RemoveStream(uint32_t stream_id);

    /// 该流的账目是否还在（测试/调试用）。
    /// 参数：stream_id - 流 ID
    bool HasStream(uint32_t stream_id) const;

private:
    // 单个实体的接收账。
    struct RecvState {
        int32_t  credit   = 0;   // 剩余可接收额度（可为负）
        uint32_t consumed = 0;   // 自上次 WINDOW_UPDATE 以来已消费的字节数
    };

    uint32_t recv_initial_ = 65535;   // 本端 SETTINGS 的 INITIAL_WINDOW_SIZE（只作用于流级）
    uint32_t send_initial_ = 65535;   // 对端 SETTINGS 的 INITIAL_WINDOW_SIZE（只作用于流级）
    // 连接级接收账的初始大小。它与流级不同，【不】受 SETTINGS_INITIAL_WINDOW_SIZE
    // 影响（§6.9.2），所以必须单独存一份：连接级的补窗口阈值要用它，不能用
    // recv_initial_。否则本端一旦广告了大接收窗口，阈值会高过连接窗口本身能
    // 消耗的上限（连接窗口上限恒为它的初始值），ShouldUpdate(0) 永假、连接级
    // WINDOW_UPDATE 永不发出 → 连接窗口耗尽后整条连接停滞。
    // 这与 h2_session.cpp 里记录的「peer 10MB → 收满 64KB 后大 body 死锁」同源。
    uint32_t conn_recv_initial_ = 65535;
    RecvState conn_recv_;             // 连接级接收账
    int32_t   conn_send_ = 65535;     // 连接级发送账

    std::unordered_map<uint32_t, RecvState> recv_;   // 流级接收账
    std::unordered_map<uint32_t, int32_t>   send_;   // 流级发送账

    // 取流级接收账，不存在则按 recv_initial_ 创建；流 ID 0 返回连接账。
    // 参数：stream_id - 流 ID（0 = 连接级）
    RecvState& GetOrCreateRecv(uint32_t stream_id);

    // 取流级发送账，不存在则按 send_initial_ 创建。
    // 参数：stream_id - 流 ID（调用方需保证非 0）
    int32_t& GetOrCreateSend(uint32_t stream_id);

    // 把 int32_t 窗口转成对外呈现的 uint32_t（负值一律当 0）。
    // 参数：v - 原始窗口值
    static uint32_t Clamp(int32_t v);
};
