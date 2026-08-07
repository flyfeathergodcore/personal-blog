#include "cache/file_cache.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// 析构：关闭所有已打开的文件描述符
FileCache::~FileCache()
{
    int closed = 0;
    for (auto& [key, file] : files_) {
        if (file.fd >= 0) {
            ::close(file.fd);
            closed++;
        }
    }
    if (closed > 0)
        std::cout << "[cache] 关闭 " << closed << " 个文件 fd" << std::endl;
}

// 递归扫描 doc_root 下所有文件并载入缓存（记录 mime/fd/大小/mtime）
// 参数：doc_root - 静态文件根目录
void FileCache::LoadDirectory(const std::string& doc_root)
{
    doc_root_ = doc_root;
    auto root = fs::absolute(doc_root);
    int count = 0, fd_count = 0;
    size_t total = 0;

    if (!fs::exists(root)) {
        std::cerr << "[cache] 目录不存在: " << root << std::endl;
        return;
    }

    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;

        auto path = entry.path();
        auto rel = fs::relative(path, root);
        std::string key = "/" + rel.generic_string();

        auto file_size = static_cast<size_t>(entry.file_size());

        // Stat for mtime
        struct stat st;
        time_t mtime = 0;
        if (::stat(path.c_str(), &st) == 0)
            mtime = st.st_mtime;

        // Open fd for sendfile (always keep open)
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            std::cerr << "[cache] 无法打开: " << path << std::endl;
            continue;
        }
        fd_count++;

        // In-memory copy for SSL fallback (only small files)
        std::string content;
        if (file_size <= kSmallFileMax) {
            std::ifstream file(path, std::ios::binary);
            if (file) {
                std::stringstream ss;
                ss << file.rdbuf();
                content = ss.str();
            }
        }

        files_[key] = CachedFile{
            std::move(content),
            DetectMime(path),
            fd,
            file_size,
            mtime
        };
        total += file_size;
        count++;
    }

    std::cout << "[cache] 加载 " << count << " 个文件，共 "
              << (total / 1024) << " KB, "
              << fd_count << " 个 fd 已打开" << std::endl;
}

// 按路径查找缓存的文件；未命中返回 nullptr
// 参数：path - 请求路径（如 "/index.html"）
const CachedFile* FileCache::Get(const std::string& path) const
{
    auto it = files_.find(path);
    return it != files_.end() ? &it->second : nullptr;
}

// 根据文件扩展名判断 MIME 类型
// 参数：p - 文件路径；返回对应 MIME 字符串
std::string FileCache::DetectMime(const fs::path& p)
{
    auto ext = p.extension().string();
    if (ext == ".html") return "text/html";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".json") return "application/json";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".svg")  return "image/svg+xml";
    return "application/octet-stream";
}
