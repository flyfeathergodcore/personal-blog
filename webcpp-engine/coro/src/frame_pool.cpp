// 协程帧分配实现：直接使用 mimalloc，避免手写全局 free-list 的锁竞争。
#include "coro/frame_pool.h"

#include <mimalloc.h>

#include <new>

namespace coro {

FramePool& FramePool::instance()
{
    static FramePool pool;
    return pool;
}

void* FramePool::alloc(std::size_t size)
{
    void* ptr = mi_malloc(size);
    if (!ptr) throw std::bad_alloc();
    instance().live_.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void* FramePool::alloc_aligned(std::size_t size, std::size_t alignment)
{
    void* ptr = mi_malloc_aligned(size, alignment);
    if (!ptr) throw std::bad_alloc();
    instance().live_.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void FramePool::free(void* ptr, std::size_t /*size*/)
{
    if (!ptr) return;
    mi_free(ptr);  // mimalloc 支持跨线程释放。
    instance().live_.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t FramePool::live_blocks()
{
    return instance().live_.load(std::memory_order_relaxed);
}

}  // namespace coro
