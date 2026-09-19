#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace fs = std::filesystem;

// 缓存项：文件内容/mime/fd/大小/mtime 的不可变快照。
// 由 shared_ptr 持有生命周期（后台刷新替换缓存项时，正在发送响应的旧对象
// 由引用计数保证存活，fd/content 不会在发送中途被回收）。
struct CachedFile {
    std::string content;   // in-memory body (empty for large files)
    std::string mime;      // MIME type
    int fd = -1;           // file descriptor for sendfile
    size_t file_size = 0;  // actual file size on disk
    time_t mtime = 0;      // modification time (for Last-Modified / ETag)

    // 析构时关闭 fd（唯一负责 fd 释放的出口；对象被替换或进程结束都走这里）
    ~CachedFile() { if (fd >= 0) ::close(fd); }

    CachedFile() = default;
    // fd 不可复制（复制会导致双 close / 悬垂），只允许移动
    CachedFile(const CachedFile&) = delete;
    CachedFile& operator=(const CachedFile&) = delete;
    CachedFile(CachedFile&& o) noexcept;
    CachedFile& operator=(CachedFile&& o) noexcept;
};

class FileCache {
public:
    // 析构：停止后台刷新线程；各缓存项 fd 由 CachedFile 析构回收
    ~FileCache();

    // 递归扫描 doc_root 下所有文件并载入缓存
    // 参数：doc_root - 静态文件根目录
    void LoadDirectory(const std::string& doc_root);
    // 按路径查找缓存文件，未命中返回 nullptr。
    // 返回 shared_ptr（线程安全）：后台刷新替换缓存时，调用方持有的旧对象
    // 由引用计数保证存活，可安全跨 Handler 返回继续使用。
    // 参数：path - 请求路径
    std::shared_ptr<const CachedFile> Get(const std::string& path) const;
    // 返回文档根目录
    const std::string& DocRoot() const { return doc_root_; }

    // 启动后台刷新线程：每 interval_sec 秒扫描 doc_root，检测文件 mtime/大小
    // 变化（含新增/删除）并更新缓存。前端 dist 更新后无需重启容器，
    // 最多一个扫描周期即生效。幂等：已在运行时直接返回。
    // 参数：interval_sec - 扫描间隔（秒），默认 5
    void StartRefresher(int interval_sec = 5);
    // 停止后台刷新线程并等待退出（析构时自动调用；幂等）
    void StopRefresher();

    /// Small-file threshold in bytes — files <= this are cached in
    /// memory; larger files keep only their fd + size.
    static constexpr size_t kSmallFileMax = 65536;

private:
    std::string doc_root_;
    // 缓存表：key = "/相对路径"，值由 shared_ptr 管理（见 CachedFile 注释）
    std::unordered_map<std::string, std::shared_ptr<CachedFile>> files_;
    mutable std::mutex mutex_;        // 保护 files_
    std::thread refresher_;           // 后台刷新线程
    std::atomic<bool> running_{false}; // 刷新线程运行标志
    int interval_sec_ = 5;

    // 扫描磁盘并增量更新缓存的一次快照（后台线程循环调用）：
    // 在锁外探测变化、加载新内容，最后加锁替换，避免磁盘 I/O 阻塞请求路径。
    void RefreshOnce();
    // 根据文件扩展名判断 MIME 类型
    // 参数：p - 文件路径；返回对应 MIME 字符串
    static std::string DetectMime(const fs::path& p);
    // 加载单个磁盘文件为一个新的缓存快照（open fd + 按需读入内存）
    // 参数：path - 磁盘路径；key - 缓存键（"/xxx"，仅用于日志）
    // 返回：新缓存项；open 失败返回 nullptr
    static std::shared_ptr<CachedFile> LoadFile(const fs::path& path,
                                                const std::string& key);
};
