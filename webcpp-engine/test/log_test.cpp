// log_test — 验证 Logger::Init / Log / StopAll 与落盘 + 级别过滤
#include "log/logger.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

static int Fail(const char* msg)
{
    std::fprintf(stderr, "LOG-TEST-FAIL: %s\n", msg);
    return 1;
}

int main()
{
    std::remove("/tmp/webcpp_log_test/business.log");
    Logger::Init("/tmp/webcpp_log_test", LogLevel::Info);
    Logger::Log(LogLevel::Info,  "TEST", "hello");
    Logger::Log(LogLevel::Error, "TEST", "boom");
    Logger::Log(LogLevel::Debug, "TEST", "hidden_debug");

    constexpr int kThreads = 8;
    constexpr int kEntriesPerThread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([i] {
            for (int j = 0; j < kEntriesPerThread; ++j) {
                Logger::Log(LogLevel::Info, "CONCURRENCY",
                            "concurrent-" + std::to_string(i) + "-" + std::to_string(j));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    Logger::StopAll();

    std::ifstream f("/tmp/webcpp_log_test/business.log");
    if (!f.is_open())
        return Fail("business.log 不存在");
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.find("boom") == std::string::npos)
        return Fail("business.log 缺少 'boom'");
    if (content.find("hello") == std::string::npos)
        return Fail("business.log 缺少 'hello'");
    if (content.find("hidden_debug") != std::string::npos)
        return Fail("Debug 日志未被级别过滤");
    std::size_t concurrent_count = 0;
    for (std::size_t pos = 0; (pos = content.find("[CONCURRENCY]", pos)) != std::string::npos;
         ++pos)
        ++concurrent_count;
    if (concurrent_count != static_cast<std::size_t>(kThreads * kEntriesPerThread))
        return Fail("并发日志未完整落盘");

    std::printf("LOG-TEST-PASS\n");
    return 0;
}
