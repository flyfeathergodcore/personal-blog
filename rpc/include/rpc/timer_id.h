// 全局单调递增的可取消定时器 id 分配器。
// EventLoop 的 live_timer_ids_ 是全局共享集合，任何调用 wait_timer_cancelable
// 的组件（WriteLock、RpcChannel 超时等）都必须使用全局唯一的 id，否则一个
// 组件的 cancel_timer 可能误伤另一个组件的定时器。这里统一分配，杜绝碰撞。
#pragma once

#include <atomic>
#include <cstdint>

namespace rpc {

// 分配下一个全局唯一的可取消定时器 id（线程安全）
inline std::uint64_t NextTimerId() {
    static std::atomic<std::uint64_t> g_id{1};
    return g_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace rpc
