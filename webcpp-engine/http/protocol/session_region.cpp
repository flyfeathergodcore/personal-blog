#include "http/protocol/session_region.hpp"
#include "http/protocol/region_pool.hpp"
#include <cstring>

// 析构：若已从池中获取区域，则归还（Release）给 RegionPool
// 参数：无
SessionRegion::~SessionRegion() {
    if (pool_ && offset_ != 0) {
        pool_->Release(offset_, cap_);
    }
}

// 初始化/绑定区域池：幂等可重复调用（已绑同池则仅重置，否则先归还旧区域再重新获取）
// 参数：pool - 所属 RegionPool；传 nullptr 表示解除绑定
void SessionRegion::Init(RegionPool* pool) {
    // If already attached to this pool with a valid region, just reset.
    if (pool_ == pool && offset_ != 0) {
        used_ = 0;
        return;
    }

    // Release any previous region first.
    if (pool_ && offset_ != 0) {
        pool_->Release(offset_, cap_);
    }

    pool_ = pool;
    if (!pool_) return;

    auto [off, cap] = pool_->Acquire(kInitSize);
    offset_ = off;
    cap_    = cap;
    used_   = 0;
}

// 复位为下一个请求：游标归零并退出结构化模式，但区域保留在池中不归还
// 参数：无
void SessionRegion::Reset() {
    used_ = 0;
    structured_mode_ = false;
}

// 从区域中按 8 字节对齐分配 n 字节（容量不足时自动 2× 迁移扩容）
// 参数：n - 请求分配字节数；返回：分配的内存指针，失败返回 nullptr
void* SessionRegion::Alloc(size_t n) {
    if (n == 0) return nullptr;

    // Round to 8-byte alignment.
    n = (n + kAlign - 1) & ~(kAlign - 1);

    // 容量不足时循环 Migrate（2× 翻倍）直到足够，或触达池上限才返回失败。
    // 此前只 Migrate 一次：请求体（H1Parser 按 Content-Length 一次性 Alloc 整块
    // body）超过 2× 当前 cap 时分配失败返回 nullptr，body 写不进去，连接无限等待。
    while (used_ + n > cap_) {
        if (cap_ >= RegionPool::kPoolSize) return nullptr;  // 已达 256MB 上限仍不够 → OOM
        Migrate();
    }

    char* ptr = Data() + used_;
    used_ += n;
    return ptr;
}

// 将字符串复制到区域内存并返回视图（容量不足自动扩容）
// 参数：s - 待复制的字符串；返回：区域内的副本视图，空串返回空
std::string_view SessionRegion::Dup(std::string_view s) {
    if (s.empty()) return {};
    char* p = static_cast<char*>(Alloc(s.size()));
    if (!p) return {};
    std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
}

// 复制字符串并返回基于偏移的引用（跨迁移存续）
// 参数：s - 待复制的字符串；返回：RegionOff{偏移, 长度}，空串返回空
RegionOff SessionRegion::DupOff(std::string_view s) {
    auto sv = Dup(s);
    if (sv.empty()) return {};
    auto off = static_cast<uint32_t>(sv.data() - Data());
    return {off, static_cast<uint32_t>(sv.size())};
}

// 扩容迁移：向池申请 2× 容量的新区块，拷贝旧数据后释放旧区域
// 参数：无
void SessionRegion::Migrate() {
    if (!pool_) return;

    // Vector-like: 2× capacity (at most one extra block per Session).
    size_t new_cap = cap_ * 2;
    if (new_cap > RegionPool::kPoolSize) new_cap = RegionPool::kPoolSize;

    auto [new_off, new_cap_actual] = pool_->Acquire(new_cap);
    if (new_off == 0) return;  // OOM

    // Copy old data to the start of the new region.
    if (used_ > 0) {
        std::memcpy(pool_->Base() + new_off, Data(), used_);
    }

    // Release old region.
    pool_->Release(offset_, cap_);

    // Switch.
    offset_ = new_off;
    cap_    = new_cap_actual;
}

// 向区域写入数据（无对齐填充，容量不足自动 2× 迁移）
// 参数：s - 待写入的字节数据
void SessionRegion::Write(std::string_view s) {
    if (s.empty()) return;
    auto n = s.size();
    // 容量不足时循环 Migrate（2× 翻倍）直到足够，触达池上限仍不够才放弃。
    // 响应 body 可能很大（如大图/大文章），只 Migrate 一次会写不下而静默丢弃，
    // 造成 Content-Length 已声明但 body 缺失、连接挂起。与 Alloc 保持一致。
    while (used_ + n > cap_) {
        if (cap_ >= RegionPool::kPoolSize) return;
        Migrate();
    }
    std::memcpy(Data() + used_, s.data(), n);
    used_ += n;
}

// 便捷写入 "\r\n" 回车换行
// 参数：无
void SessionRegion::WriteCRLF() {
    Write("\r\n");
}

// 以十进制无分配方式写入无符号整数
// 参数：n - 待写入的整数
void SessionRegion::WriteUint(uint64_t n) {
    char tmp[24];
    char* p = tmp + sizeof(tmp);
    *--p = '\0';
    if (n == 0) { *--p = '0'; }
    else {
        while (n > 0) {
            *--p = char('0' + (n % 10));
            n /= 10;
        }
    }
    Write(p);
}
