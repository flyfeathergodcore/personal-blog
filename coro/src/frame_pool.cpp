// 协程帧内存池实现：分桶 free list + 全局静态 mutex（协程可能任意线程创建/销毁）
#include "coro/frame_pool.h"

#include <cstdlib>
#include <mutex>

namespace coro {

constexpr std::size_t FramePool::kBucketCapacity[];
constexpr int FramePool::kNumBuckets;

namespace {
std::mutex g_pool_mu;
}  // namespace

// 获取内存池单例（首次调用时构造）
FramePool& FramePool::instance() {
    static FramePool pool;
    return pool;
}

// 分配至少 size 字节的内存（协程帧）；优先走分桶 free list，超大块直接 malloc
// 参数：size - 需要的字节数；返回：内存指针（失败为 nullptr）
void* FramePool::alloc(std::size_t size) {
    // 找最小够用的桶
    for (int i = 0; i < kNumBuckets; ++i) {
        const std::size_t usable = kBucketCapacity[i] - sizeof(BlockHeader);
        if (size <= usable) {
            auto& inst = instance();
            std::lock_guard<std::mutex> lock(g_pool_mu);
            BlockHeader* b = inst.free_lists_[i];
            if (b) {
                inst.free_lists_[i] = b->next;
                inst.live_ += 1;
                return reinterpret_cast<char*>(b) + sizeof(BlockHeader);
            }
            void* raw = std::malloc(kBucketCapacity[i]);
            if (!raw) return nullptr;
            b = static_cast<BlockHeader*>(raw);
            b->size = kBucketCapacity[i];
            inst.live_ += 1;
            return reinterpret_cast<char*>(b) + sizeof(BlockHeader);
        }
    }
    // 超过最大桶：直接 malloc 并带头，头内 size=0 标记"非池大块"。
    // 不带头的话 free() 无法区分大块与池块，会读取 malloc 区域前的越界内存。
    // 大块不计入 live_（与设计一致）。
    void* raw = std::malloc(size + sizeof(BlockHeader));
    if (!raw) return nullptr;
    BlockHeader* bh = static_cast<BlockHeader*>(raw);
    bh->size = 0;
    bh->next = nullptr;
    return reinterpret_cast<char*>(bh) + sizeof(BlockHeader);
}

// 释放由 alloc 分配的内存（经头记录区分池块与大块）
// 参数：ptr - 待释放指针；size - 原始分配大小（可忽略）
void FramePool::free(void* ptr, std::size_t /*size*/) {
    if (!ptr) return;
    BlockHeader* b = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<char*>(ptr) - sizeof(BlockHeader));
    // 大块标记（size=0）：头即 malloc 基址，直接释放
    if (b->size == 0) {
        std::free(b);
        return;
    }
    // 从头部记录判断是否来自桶
    int idx = -1;
    for (int i = 0; i < kNumBuckets; ++i) {
        if (b->size == kBucketCapacity[i]) { idx = i; break; }
    }
    if (idx >= 0) {
        auto& inst = instance();
        std::lock_guard<std::mutex> lock(g_pool_mu);
        b->next = inst.free_lists_[idx];
        inst.free_lists_[idx] = b;
        inst.live_ -= 1;
        return;
    }
    std::free(ptr);
}

// 测试用：返回当前活跃（未释放）的池内块数；>4KB 大块不计入
std::size_t FramePool::live_blocks() {
    std::lock_guard<std::mutex> lock(g_pool_mu);
    return instance().live_;
}

}  // namespace coro
