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
// 复位对象：最高优先级，调用自定义 Reset()
// 参数：obj - 待复位对象
template<typename T>
auto CallReset(T& obj, int) -> decltype(obj.Reset(), void()) { obj.Reset(); }

// 复位对象：次优先级，调用 protobuf Message 的 Clear()
// 参数：obj - 待复位对象
template<typename T>
auto CallReset(T& obj, long) -> decltype(obj.Clear(), void()) { obj.Clear(); }

// 复位对象：兜底，调用 STL 容器的 clear()
// 参数：obj - 待复位对象
template<typename T>
auto CallReset(T& obj, ...) -> decltype(obj.clear(), void()) { obj.clear(); }

} // namespace detail

template<typename T, size_t Capacity = 32>
class ObjectPool {
public:
    // RAII 句柄：析构时自动归还对象
    class PooledObject {
    public:
        // 默认构造（空句柄）
        PooledObject() = default;
        // 构造 RAII 句柄：绑定被管理对象与所属对象池
        // 参数：obj - 被管理的对象指针；pool - 所属对象池
        PooledObject(T* obj, ObjectPool* pool)
            : obj_(obj), pool_(pool) {}

        // 禁止拷贝构造
        PooledObject(const PooledObject&) = delete;
        // 禁止拷贝赋值
        PooledObject& operator=(const PooledObject&) = delete;

        // 允许移动构造（转移所有权，源句柄置空）
        // 参数：o - 被移动的句柄
        PooledObject(PooledObject&& o) noexcept
            : obj_(o.obj_), pool_(o.pool_) {
            o.obj_ = nullptr;
        }
        // 移动赋值（先归还旧对象，再转移所有权）
        // 参数：o - 被移动的句柄
        PooledObject& operator=(PooledObject&& o) noexcept {
            if (this != &o) {
                release();
                obj_ = o.obj_;
                pool_ = o.pool_;
                o.obj_ = nullptr;
            }
            return *this;
        }

        // 析构：自动复位并归还对象到池
        ~PooledObject() { release(); }

        // 指针访问（非 const）
        T* operator->() { return obj_; }
        // 解引用访问（非 const）
        T& operator*()  { return *obj_; }
        // 指针访问（const）
        const T* operator->() const { return obj_; }
        // 解引用访问（const）
        const T& operator*()  const { return *obj_; }

        // 判断句柄是否持有有效对象
        // 参数：无
        bool valid() const { return obj_ != nullptr; }

    private:
        // 复位并归还对象到池，随后置空指针（析构与移动赋值共用）
        // 参数：无
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

    // 默认构造对象池
    ObjectPool() = default;
    // 析构：删除池中所有空闲对象
    ~ObjectPool() {
        for (size_t i = 0; i < top_; i++) {
            delete stack_[i];
        }
    }

    // 禁止拷贝
    ObjectPool(const ObjectPool&) = delete;
    // 禁止拷贝赋值
    ObjectPool& operator=(const ObjectPool&) = delete;

    // 从池中获取对象（池空则新建），返回 RAII 句柄
    // 参数：无；返回：PooledObject 句柄（离开作用域自动复位归还）
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

    // 统计：已创建对象总数（含池满被删除的）
    // 参数：无
    size_t Created() const { return created_.load(std::memory_order_relaxed); }

    // 统计：复用次数
    // 参数：无
    size_t Reused() const { return reused_.load(std::memory_order_relaxed); }

    // 当前空闲对象数
    // 参数：无
    size_t FreeCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return top_;
    }

private:
    // 归还对象到池：池未满入栈复用，池满直接删除防止无限增长
    // 参数：obj - 待归还的对象指针
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
