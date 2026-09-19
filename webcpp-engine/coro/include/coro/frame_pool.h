// 协程帧分配器：保留 FramePool 接口，底层直接委托 mimalloc。
// mimalloc 的线程本地 heap、大小分级和跨线程释放优于全局 free-list，
// 适用于多 reactor 并发创建/销毁协程帧的场景。
#pragma once

#include <atomic>
#include <cstddef>

namespace coro {

class FramePool {
public:
    // 分配一块至少 size 字节的内存（协程帧）；失败时抛 std::bad_alloc。
    static void* alloc(std::size_t size);
    // 分配指定对齐的协程帧。
    static void* alloc_aligned(std::size_t size, std::size_t alignment);
    // 释放由 alloc/alloc_aligned 分配的内存（size 可忽略）。
    static void free(void* ptr, std::size_t size);
    // 测试/诊断用：当前活跃（未释放）的协程帧数。
    static std::size_t live_blocks();

private:
    // 获取诊断计数器单例；分配状态完全由 mimalloc 管理。
    static FramePool& instance();

    std::atomic<std::size_t> live_{0};
};

}  // namespace coro
