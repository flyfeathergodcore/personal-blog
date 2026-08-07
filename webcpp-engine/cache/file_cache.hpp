#pragma once
#include <string>
#include <unordered_map>
#include <filesystem>

namespace fs = std::filesystem;

struct CachedFile {
    std::string content;   // in-memory body (empty for large files)
    std::string mime;      // MIME type
    int fd = -1;           // file descriptor for sendfile
    size_t file_size = 0;  // actual file size on disk
    time_t mtime = 0;      // modification time (for Last-Modified / ETag)
};

class FileCache {
public:
    // 析构：关闭所有已打开的文件描述符
    ~FileCache();

    // 递归扫描 doc_root 下所有文件并载入缓存
    // 参数：doc_root - 静态文件根目录
    void LoadDirectory(const std::string& doc_root);
    // 按路径查找缓存文件，未命中返回 nullptr
    // 参数：path - 请求路径
    const CachedFile* Get(const std::string& path) const;
    // 返回文档根目录
    const std::string& DocRoot() const { return doc_root_; }

    /// Small-file threshold in bytes — files <= this are cached in
    /// memory; larger files keep only their fd + size.
    static constexpr size_t kSmallFileMax = 65536;

private:
    std::string doc_root_;
    std::unordered_map<std::string, CachedFile> files_;
    // 根据文件扩展名判断 MIME 类型
    // 参数：p - 文件路径；返回对应 MIME 字符串
    static std::string DetectMime(const fs::path& p);
};
