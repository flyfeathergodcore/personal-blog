#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>

// ═══════════════════════════════════════════════════════════════
// ObjectPool<T, Capacity>
//
// 通用对象复用池。Acquire() 从空闲链表取出对象（已调用 Reset()），
// 对象析构时自动归还。适合频繁构造/析构的对象：
//   - protobuf Message（ChatClientMessage、QueryRequest 等）
//   - grpc::ClientContext
//   - nlohmann::json
//
// 使用方式：
//   static ObjectPool<ai::chat::ChatClientMessage> pool;
//   auto msg = pool.Acquire();    // PooledObject<T> RAII 包装
//   msg->set_session_id("xxx");   // 用 -> 操作对象
//   // 离开作用域后自动 Clear() 归还
//
// 线程安全：spinlock 保护空闲栈，适合多 worker 共享同一个 pool。
//
// 对象必须满足以下接口之一（优先级从高到低）：
//   1. Reset()   — 自定义复位
//   2. Clear()   — protobuf Message 标准接口
//   3. clear()   — STL 容器标准接口
// ═══════════════════════════════════════════════════════════════

namespace detail {

// 探测复位函数：优先 Reset() → Clear() → clear()
template<typename T>
auto CallReset(T& obj, int) -> decltype(obj.Reset(), void()) { obj.Reset(); }

template<typename T>
auto CallReset(T& obj, long) -> decltype(obj.Clear(), void()) { obj.Clear(); }

template<typename T>
auto CallReset(T& obj, ...) -> decltype(obj.clear(), void()) { obj.clear(); }

} // namespace detail

template<typename T, size_t Capacity = 32>
class ObjectPool {
public:
    // RAII 句柄：析构时自动归还对象
    class PooledObject {
    public:
        PooledObject() = default;
        PooledObject(T* obj, ObjectPool* pool)
            : obj_(obj), pool_(pool) {}

        // 禁止拷贝
        PooledObject(const PooledObject&) = delete;
        PooledObject& operator=(const PooledObject&) = delete;

        // 允许移动
        PooledObject(PooledObject&& o) noexcept
            : obj_(o.obj_), pool_(o.pool_) {
            o.obj_ = nullptr;
        }
        PooledObject& operator=(PooledObject&& o) noexcept {
            if (this != &o) {
                release();
                obj_ = o.obj_;
                pool_ = o.pool_;
                o.obj_ = nullptr;
            }
            return *this;
        }

        ~PooledObject() { release(); }

        T* operator->() { return obj_; }
        T& operator*()  { return *obj_; }
        const T* operator->() const { return obj_; }
        const T& operator*()  const { return *obj_; }

        bool valid() const { return obj_ != nullptr; }

    private:
        void release() {
            if (obj_ && pool_) {
                detail::CallReset(*obj_, 0);
                pool_->Return(obj_);
                obj_ = nullptr;
            }
        }

        T*           obj_  = nullptr;
        ObjectPool*  pool_ = nullptr;
    };

    ObjectPool() = default;
    ~ObjectPool() {
        for (size_t i = 0; i < top_; i++) {
            delete stack_[i];
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // 从池中获取对象（池空则新建），返回 RAII 句柄
    PooledObject Acquire() {
        T* obj = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (top_ > 0) {
                obj = stack_[--top_];
            }
        }
        if (!obj) {
            obj = new T();
            created_.fetch_add(1, std::memory_order_relaxed);
        } else {
            reused_.fetch_add(1, std::memory_order_relaxed);
        }
        return {obj, this};
    }

    // 统计：已创建对象总数
    size_t Created() const { return created_.load(std::memory_order_relaxed); }

    // 统计：复用次数
    size_t Reused() const { return reused_.load(std::memory_order_relaxed); }

    // 当前空闲对象数
    size_t FreeCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return top_;
    }

private:
    void Return(T* obj) {
        std::lock_guard<std::mutex> lock(mu_);
        if (top_ < Capacity) {
            stack_[top_++] = obj;
        } else {
            // 池满，直接删除（防止无限增长）
            delete obj;
        }
    }

    mutable std::mutex        mu_;
    std::array<T*, Capacity>  stack_{};
    size_t                    top_ = 0;

    std::atomic<size_t> created_{0};
    std::atomic<size_t> reused_{0};
};
