// log_test — 验证 Logger::Init / Log / StopAll 与落盘 + 级别过滤
#include "log/logger.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static int Fail(const char* msg)
{
    std::fprintf(stderr, "LOG-TEST-FAIL: %s\n", msg);
    return 1;
}

int main()
{
    Logger::Init("/tmp/webcpp_log_test", LogLevel::Info);
    Logger::Log(LogLevel::Info,  "TEST", "hello");
    Logger::Log(LogLevel::Error, "TEST", "boom");
    Logger::Log(LogLevel::Debug, "TEST", "hidden_debug");
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

    std::printf("LOG-TEST-PASS\n");
    return 0;
}
