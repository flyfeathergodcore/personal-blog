#include "http/protocol/region_pool.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

// 构造：通过 mmap 一次性预分配 256MB 虚拟内存作为区域池
// 参数：无
RegionPool::RegionPool() {
    void* p = mmap(nullptr, kPoolSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        std::cerr << "[region_pool] mmap(" << (kPoolSize / 1024 / 1024)
                  << " MB) failed: " << strerror(errno) << std::endl;
        p = nullptr;
    }
    base_ = static_cast<char*>(p);
    // fprintf(stderr, "[region_pool] mmap %zu MB at %p\n",
    //         kPoolSize / 1024 / 1024, p);
}

// 析构：munmap 释放预分配的 256MB 内存
// 参数：无
RegionPool::~RegionPool() {
    if (base_) {
        munmap(base_, kPoolSize);
    }
}

// 获取一块至少 min_size 字节的区域（优先空闲链表 first-fit，后备 bump 分配）
// 参数：min_size - 最小需求字节数；返回：{池内偏移, 实际容量}，失败为 {0,0}
std::pair<size_t, size_t> RegionPool::Acquire(size_t min_size) {
    // Round up to kMinRegion alignment for freelist simplicity.
    size_t need = ((min_size + kMinRegion - 1) / kMinRegion) * kMinRegion;
    if (need < kMinRegion) need = kMinRegion;

    // First-fit through freelist.
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->size >= need) {
            auto result = std::make_pair(it->offset, it->size);
            free_.erase(it);
            return result;
        }
    }

    // Bump allocate.
    size_t offset = bump_offset_;
    bump_offset_ += need;
    if (bump_offset_ > kPoolSize) {
        std::cerr << "[region_pool] OOM: need " << need
                  << ", bump " << bump_offset_ << " > " << kPoolSize << std::endl;
        return {0, 0};  // error
    }
    return {offset, need};
}

// 归还区域到空闲链表，并触发相邻空闲块合并
// 参数：offset - 池内偏移；size - 区域大小
void RegionPool::Release(size_t offset, size_t size) {
    if (size == 0) return;
    free_.push_back({offset, size});
    Coalesce();
}

// 合并空闲链表中地址相邻的空闲块（每次 Release 后调用）
// 参数：无
void RegionPool::Coalesce() {
    if (free_.size() < 2) return;

    // Sort by offset.
    std::sort(free_.begin(), free_.end(),
              [](const FreeSlot& a, const FreeSlot& b) {
                  return a.offset < b.offset;
              });

    // Merge adjacent.
    size_t wi = 0;
    for (size_t ri = 1; ri < free_.size(); ++ri) {
        auto& prev = free_[wi];
        auto& curr = free_[ri];
        if (prev.offset + prev.size == curr.offset) {
            prev.size += curr.size;
        } else {
            ++wi;
            if (wi != ri) free_[wi] = curr;
        }
    }
    free_.resize(wi + 1);
}
