#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <filesystem>
#include <dlfcn.h>

class Router;

// ═══════════════════════════════════════════════════════════════════
// HotReloader — inotify 监控 .so 目录，文件变化时自动热重载
//
// 在后台线程运行，检测到 .so 发生变化后：
//   1. dlopen 新版本
//   2. dlsym("register_routes") 获取注册函数
//   3. 调用注册函数更新路由
//   4. 旧 .so 保持打开（避免正在执行的 handler 崩溃）
//
// 注意：Router::Add 和 Router::Match 在多线程时需要外部同步。
// 此实现假设在开发环境下，不存在大量的并发请求。
// ═══════════════════════════════════════════════════════════════════

class HotReloader {
public:
    // 构造热重载器
    // 参数：watch_dirs - 要监控的 .so 目录列表；router - 热更新时写入路由的目标 Router
    HotReloader(std::vector<std::string> watch_dirs, Router& router);
    // 析构时停止后台线程并清理资源
    ~HotReloader();

    HotReloader(const HotReloader&) = delete;
    HotReloader& operator=(const HotReloader&) = delete;

    // 启动后台监控线程
    void Start();
    // 停止后台监控线程
    void Stop();

private:
    // 后台线程主循环：轮询目录，检测 .so 变化并触发重载
    void WatchLoop();

    struct LoadedSo {
        void* handle = nullptr;
        std::filesystem::file_time_type mtime;
    };

    std::vector<std::string> watch_dirs_;
    Router& router_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    uint64_t version_ = 0;  // 临时文件版本后缀

    std::unordered_map<std::string, LoadedSo> loaded_;
    std::vector<void*> stale_handles_;  // 旧 .so，保持打开直到进程退出

    // 重载单个 .so：dlopen 新版本并调用 register_routes 更新路由
    // 参数：path - .so 文件路径
    void Reload(const std::string& path);
};
