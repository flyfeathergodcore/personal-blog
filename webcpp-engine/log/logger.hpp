#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include <ctime>
#include <string_view>

// ═══════════════════════════════════════════════════════════════════
// 三层日志系统
//
// Gateway    — HTTP 请求日志 (logs/gateway.log)
// Business   — 业务日志 (logs/business.log)
// Perf       — 性能日志 (logs/perf.log)
// ═══════════════════════════════════════════════════════════════════

// 日志级别（阈值模型：Debug=0 … Error=3，Log 写入当且仅当 level >= 当前级别）
enum class LogLevel { Debug, Info, Warn, Error };

// ── 无锁异步日志写入器（每个分类一个） ──
class LogWriter {
    struct Chunk {
        static constexpr size_t MAX = 4096;
        std::vector<std::string> entries;
        Chunk() { entries.reserve(MAX); }
        bool Full() const { return entries.size() >= MAX; }
        bool Empty() const { return entries.empty(); }
    };

    std::string filename_;
    std::ofstream file_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false}, started_{false};
    std::atomic<uint64_t> flush_epoch_{0}, last_flushed_epoch_{0};
    std::vector<std::unique_ptr<Chunk>> free_;
    std::vector<Chunk*> ready_;
    std::thread writer_;
    static inline thread_local Chunk* tls_q_ = nullptr;
    static inline thread_local uint64_t tls_epoch_ = 0;

    void Start();
    Chunk* GetQueue();
    void Submit(Chunk* q);
    void Loop();

public:
    LogWriter(const std::string& filename) : filename_(filename) {}
    ~LogWriter() { Stop(); }
    void Write(std::string msg);
    void Stop();
};

// ═══════════════════════════════════════════════════════════════════
// LogWriter 实现
// ═══════════════════════════════════════════════════════════════════

inline void LogWriter::Start()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_.load()) return;
    auto parent = std::filesystem::path(filename_).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    file_.open(filename_, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "[log] 无法打开 " << filename_ << std::endl;
        return;
    }
    running_.store(true, std::memory_order_release);
    writer_ = std::thread(&LogWriter::Loop, this);
    while (!started_.load(std::memory_order_acquire))
        std::this_thread::yield();
}

inline LogWriter::Chunk* LogWriter::GetQueue()
{
    if (!tls_q_) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!free_.empty()) {
            tls_q_ = free_.back().release();
            free_.pop_back();
        } else { tls_q_ = new Chunk(); }
    }
    return tls_q_;
}

inline void LogWriter::Submit(Chunk* q)
{
    std::lock_guard<std::mutex> lock(mtx_);
    ready_.push_back(q);
    tls_q_ = nullptr;
    cv_.notify_one();
}

inline void LogWriter::Loop()
{
    std::vector<Chunk*> local;
    std::vector<Chunk*> recycle;
    flush_epoch_.fetch_add(1, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
    for (;;) {
        flush_epoch_.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !ready_.empty() || !running_.load();
            });
            if (!running_.load() && ready_.empty()) break;
            local.swap(ready_);
        }
        for (auto* c : local) {
            for (auto& e : c->entries) file_ << e << '\n';
            c->entries.clear(); c->entries.reserve(Chunk::MAX);
            recycle.push_back(c);
        }
        local.clear(); file_.flush();
        if (!recycle.empty()) {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto* c : recycle)
                if (free_.size() < 8) free_.emplace_back(c); else delete c;
            recycle.clear();
        }
        last_flushed_epoch_.store(
            flush_epoch_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }
}

inline void LogWriter::Write(std::string msg)
{
    if (!running_.load(std::memory_order_acquire))
        Start();
    auto* q = GetQueue();
    q->entries.push_back(std::move(msg));
    if (q->Full() || !running_.load(std::memory_order_relaxed)) {
        Submit(q); return;
    }
    uint64_t fe = flush_epoch_.load(std::memory_order_relaxed);
    if (fe != tls_epoch_) {
        tls_epoch_ = fe;
        if (!q->Empty()) Submit(q);
    }
}

inline void LogWriter::Stop()
{
    { std::lock_guard<std::mutex> lock(mtx_);
      if (!running_.exchange(false)) return; }
    if (tls_q_ && !tls_q_->Empty()) Submit(tls_q_);
    else if (tls_q_) tls_q_ = nullptr;
    cv_.notify_one();
    if (writer_.joinable()) writer_.join();
    { std::lock_guard<std::mutex> lock(mtx_); free_.clear(); }
    if (file_.is_open()) file_.close();
}

// ═══════════════════════════════════════════════════════════════════
// Logger — 三层日志统一入口
// ═══════════════════════════════════════════════════════════════════

class Logger {
public:
    static Logger& Instance() { static Logger inst; return inst; }

    // 配置日志目录与级别；默认（未 Init）等价于 Init("./logs", LogLevel::Info)。
    //
    // 线程安全：可随时调用，即便日志线程已在运行。级别立即生效；
    // 目录仅在首个写入器创建前生效（写入器惰性创建且存活到进程退出，
    // Init 不销毁已有 writer）——因此 Init 应在首次写日志前调用以改变目录。
    static void Init(const std::string& dir, LogLevel level)
    {
        Instance().init(dir, level);
    }

    // 分级日志：level >= 当前级别时写入 business.log；
    // WARN/ERROR 额外写到 stderr。
    static void Log(LogLevel level, const std::string& module,
                    const std::string& msg)
    {
        Instance().log(level, module, msg);
    }

    // 当前日志级别
    static LogLevel Level() { return Instance().level_.load(std::memory_order_relaxed); }

    // 写入 "YYYY-MM-DD HH:MM:SS.mmm"（无方括号）到 buf，返回写入长度。
    // 秒级前缀按线程缓存：同一秒内的多次调用只做一次 localtime_r/strftime，
    // 高频请求日志的主要格式化开销由此消除。
    static int AppendTimestamp(char* buf, std::size_t cap) {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        auto t = system_clock::to_time_t(now);
        thread_local std::time_t cached_t = 0;
        thread_local char cached_base[24] = "";
        if (t != cached_t) {
            std::tm tm;
            localtime_r(&t, &tm);
            std::strftime(cached_base, sizeof(cached_base), "%Y-%m-%d %H:%M:%S", &tm);
            cached_t = t;
        }
        return std::snprintf(buf, cap, "%s.%03ld", cached_base, (long)ms.count());
    }

    static std::string Timestamp() {
        char buf[40];
        AppendTimestamp(buf, sizeof(buf));
        return buf;
    }

    // 网关访问日志。string_view 参数：中间件可直接传 ctx.Method()/Path()，
    // 避免每请求构造两个临时 std::string（热路径上的一次堆分配）。
    void Gateway(std::string_view method, std::string_view path,
                 int status, uint64_t latency_us,
                 std::string_view client_ip = {})
    {
        char buf[192];
        buf[0] = '[';
        int n = AppendTimestamp(buf + 1, sizeof(buf) - 2);
        buf[n + 1] = ']';
        n += 2;
        n += std::snprintf(buf + n, sizeof(buf) - n, " %.*s %.*s %d %.1fms %.*s",
            static_cast<int>(method.size()), method.data(),
            static_cast<int>(path.size()), path.data(),
            status, latency_us / 1000.0,
            static_cast<int>(client_ip.size()), client_ip.data());
        gw().Write(std::string(buf, static_cast<std::size_t>(n)));
    }

    void Business(const std::string& module, const std::string& action,
                  const std::string& detail)
    {
        std::string msg = "[" + Timestamp() + "] [" + module + "] "
                        + action + " " + detail;
        biz().Write(msg);
    }

    void Perf(uint64_t total_req, uint64_t total_latency_us,
              int active_workers, int total_workers,
              uint64_t mem_kb)
    {
        double avg_ms = total_req > 0
            ? (total_latency_us / (double)total_req) / 1000.0 : 0.0;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "[%s] req=%llu avg=%.2fms workers=%d/%d mem=%lluKB",
            Timestamp().c_str(),
            (unsigned long long)total_req, avg_ms,
            active_workers, total_workers,
            (unsigned long long)mem_kb);
        perf().Write(buf);
    }

    static void StopAll() { Instance().stopAllImpl(); }

private:
    Logger() = default;
    ~Logger() { stopAllImpl(); }
    Logger(const Logger&) = delete;

    void stopAllImpl()
    {
        // accessor 初始化只锁一次、此后无锁返回，无需在此持锁。
        // 仅 Stop 不销毁：writer 存活到进程退出，避免 use-after-free。
        gw().Stop();
        biz().Stop();
        perf().Stop();
    }

    void init(const std::string& dir, LogLevel level)
    {
        {
            std::lock_guard<std::mutex> lock(init_mtx_);
            dir_ = dir.empty() ? "./logs" : dir;
        }
        level_.store(level, std::memory_order_relaxed);
    }

    void log(LogLevel level, const std::string& module, const std::string& msg)
    {
        if (level < level_.load(std::memory_order_relaxed)) return;  // 阈值过滤
        std::string line = "[" + Timestamp() + "] [" + LevelName(level)
                         + "] [" + module + "] " + msg;
        biz().Write(line);
        if (level >= LogLevel::Warn) {
            std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);
        }
    }

    static const char* LevelName(LogLevel l)
    {
        switch (l) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
        }
        return "INFO";
    }

    // 每个 writer 只创建一次且存活到进程退出。指针本身是 magic static（平凡析构，
    // 退出时不执行析构），所指 writer 泄漏到进程退出——与"writer 存活到进程退出"
    // 的文档一致，也避免静态析构顺序问题（Logger 静态对象可能晚于 writer 析构，
    // ~Logger 的 stopAllImpl 仍需安全访问）。初始化内部锁一次读 dir_，此后无锁
    // 返回——取代旧实现"每次调用锁 init_mtx_"，后者是每请求日志热路径上的
    // 全局锁争抢点。
    LogWriter& gw() {
        static LogWriter* w = [this] {
            std::lock_guard<std::mutex> lock(init_mtx_);
            return new LogWriter(dir_ + "/gateway.log");
        }();
        return *w;
    }
    LogWriter& biz() {
        static LogWriter* w = [this] {
            std::lock_guard<std::mutex> lock(init_mtx_);
            return new LogWriter(dir_ + "/business.log");
        }();
        return *w;
    }
    LogWriter& perf() {
        static LogWriter* w = [this] {
            std::lock_guard<std::mutex> lock(init_mtx_);
            return new LogWriter(dir_ + "/perf.log");
        }();
        return *w;
    }

    std::string dir_ = "./logs";
    std::atomic<LogLevel> level_{LogLevel::Info};  // 无锁读，Init 可并发更新
    std::mutex  init_mtx_;
};
