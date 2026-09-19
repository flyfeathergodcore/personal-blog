#include "http/cache/file_cache.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>

// 移动构造：转移 fd 所有权，源对象 fd 置 -1（避免双 close）
CachedFile::CachedFile(CachedFile&& o) noexcept
    : content(std::move(o.content))
    , mime(std::move(o.mime))
    , fd(o.fd)
    , file_size(o.file_size)
    , mtime(o.mtime)
{
    o.fd = -1;
}

// 移动赋值：先关闭自己已持有的 fd，再接管源对象资源
CachedFile& CachedFile::operator=(CachedFile&& o) noexcept
{
    if (this != &o) {
        if (fd >= 0) ::close(fd);
        content   = std::move(o.content);
        mime      = std::move(o.mime);
        fd        = o.fd;
        file_size = o.file_size;
        mtime     = o.mtime;
        o.fd = -1;
    }
    return *this;
}

// 析构：停止后台刷新线程；缓存项 fd 由 CachedFile 析构统一回收
FileCache::~FileCache()
{
    StopRefresher();
}

// 加载单个磁盘文件为一个新的缓存快照（open fd + 按需读入内存）
// 参数：path - 磁盘路径；key - 缓存键（"/xxx"，仅用于日志）
// 返回：新缓存项；open 失败返回 nullptr
std::shared_ptr<CachedFile> FileCache::LoadFile(const fs::path& path,
                                                const std::string& key)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        std::cerr << "[cache] stat 失败: " << path << std::endl;
        return nullptr;
    }

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "[cache] 无法打开: " << path << std::endl;
        return nullptr;
    }

    auto f = std::make_shared<CachedFile>();
    f->mime      = DetectMime(path);
    f->fd        = fd;
    f->file_size = static_cast<size_t>(st.st_size);
    f->mtime     = st.st_mtime;

    // In-memory copy for SSL fallback (only small files)
    if (f->file_size <= kSmallFileMax) {
        std::ifstream file(path, std::ios::binary);
        if (file) {
            std::stringstream ss;
            ss << file.rdbuf();
            f->content = ss.str();
        }
    }
    return f;
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

    std::unordered_map<std::string, std::shared_ptr<CachedFile>> fresh;
    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;

        auto path = entry.path();
        auto rel = fs::relative(path, root);
        std::string key = "/" + rel.generic_string();

        auto f = LoadFile(path, key);
        if (!f) continue;
        if (f->fd >= 0) fd_count++;
        total += f->file_size;
        count++;
        fresh[std::move(key)] = std::move(f);
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        files_ = std::move(fresh);
    }

    std::cout << "[cache] 加载 " << count << " 个文件，共 "
              << (total / 1024) << " KB, "
              << fd_count << " 个 fd 已打开" << std::endl;
}

// 按路径查找缓存的文件；未命中返回 nullptr
// 参数：path - 请求路径（如 "/index.html"）
// 返回：shared_ptr 持有的缓存快照（引用计数保证旧对象在发送期间存活）
std::shared_ptr<const CachedFile> FileCache::Get(const std::string& path) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = files_.find(path);
    return it != files_.end() ? it->second : nullptr;
}

// 扫描磁盘并增量更新缓存的一次快照：比较 mtime/大小，变化/新增项重新加载，
// 磁盘已删除的缓存项移除。磁盘 I/O 在锁外执行，最后加锁替换，避免阻塞请求。
void FileCache::RefreshOnce()
{
    if (doc_root_.empty()) return;
    std::error_code ec;
    fs::path root = doc_root_;

    // ── 1. 收集磁盘当前文件清单（key → 大小/mtime）──
    struct DiskFile {
        std::string key;
        fs::path    path;
        size_t      size;
        time_t      mtime;
    };
    std::vector<DiskFile> disk;
    if (fs::exists(root)) {
        for (auto& entry : fs::recursive_directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            struct stat st;
            if (::stat(entry.path().c_str(), &st) != 0) continue;
            auto rel = fs::relative(entry.path(), root);
            disk.push_back({"/" + rel.generic_string(), entry.path(),
                            static_cast<size_t>(st.st_size), st.st_mtime});
        }
    }

    // ── 2. 对比缓存，找出变化/新增项（仅锁内做比较，开销极小）──
    std::vector<DiskFile> to_load;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& d : disk) {
            auto it = files_.find(d.key);
            if (it == files_.end() ||
                it->second->mtime != d.mtime ||
                it->second->file_size != d.size)
                to_load.push_back(d);
        }
    }

    // ── 3. 锁外加载新内容（磁盘 I/O 不进锁）──
    for (auto& d : to_load) {
        auto nf = LoadFile(d.path, d.key);
        if (!nf) continue;
        std::lock_guard<std::mutex> lk(mutex_);
        files_[d.key] = std::move(nf);
    }

    // ── 4. 删除检测：磁盘上已不存在的缓存项移除（旧 fd 由 shared_ptr 回收）──
    if (!disk.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = files_.begin(); it != files_.end();) {
            auto found = false;
            for (auto& d : disk) {
                if (d.key == it->first) { found = true; break; }
            }
            it = found ? ++it : files_.erase(it);
        }
    }
}

// 启动后台刷新线程：每 interval_sec 秒执行一次 RefreshOnce 快照
// 参数：interval_sec - 扫描间隔（秒），默认 5
void FileCache::StartRefresher(int interval_sec)
{
    if (running_.exchange(true)) return;   // 已在运行（幂等）
    interval_sec_ = interval_sec > 0 ? interval_sec : 5;
    refresher_ = std::thread([this]() {
        while (running_.load()) {
            // 分段 sleep 以便 Stop 能及时打断
            for (int i = 0; i < interval_sec_ && running_.load(); i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!running_.load()) break;
            RefreshOnce();
        }
    });
}

// 停止后台刷新线程并等待其退出（幂等）
void FileCache::StopRefresher()
{
    if (!running_.exchange(false)) return;
    if (refresher_.joinable())
        refresher_.join();
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
