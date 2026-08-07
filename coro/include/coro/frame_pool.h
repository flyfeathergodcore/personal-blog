// 协程帧内存池：按大小分桶的 free list，协程帧统一从池分配，避免每次 operator new
#pragma once

#include <cstddef>

namespace coro {

class FramePool {
public:
    // 分配一块至少 size 字节的内存（协程帧）
    static void* alloc(std::size_t size);
    // 释放由 alloc 分配的内存（size 为原始分配大小，可忽略，头内记录）
    static void free(void* ptr, std::size_t size);
    // 测试用：当前活跃（未释放）的池内块数；>4KB 直接走 malloc 的不计入
    static std::size_t live_blocks();

private:
    // 桶容量：块总大小（含头），数据区 = 容量 - sizeof(BlockHeader)
    static constexpr std::size_t kBucketCapacity[] = {256, 512, 1024, 2048, 4096};
    static constexpr int kNumBuckets = 5;

    // C3：头自身 16 字节对齐（alignas(16)），结合 malloc 保证的 16 字节基址，
    // 数据区（基址 + sizeof(BlockHeader)）保持 16 字节对齐。否则数据区只保证
    // 8 字节对齐，帧含 alignas(16)/long double 成员时未对齐访问是 UB。
    // sizeof(BlockHeader) 仍为 16（两个 8 字节指针），桶容量与 usable 计算不变。
    struct alignas(16) BlockHeader {
        BlockHeader* next;   // free list 链接
        std::size_t size;    // 块总大小（桶容量）
    };

    static FramePool& instance();

    BlockHeader* free_lists_[kNumBuckets] = {};
    std::size_t live_ = 0;
    void* mu_slot_;  // 占位，实际用静态 mutex（见 .cpp）
};

}  // namespace coro
