#include "tcp/tcp_log.hpp"

#include "quill/LogMacros.h"
#include "quill/SimpleSetup.h"

#include <mutex>

namespace tcp {
namespace {

quill::Logger* LoggerInstance()
{
    static std::once_flag once;
    static quill::Logger* logger = nullptr;
    std::call_once(once, [] {
        // simple_logger 使用 Quill 后台线程和无锁前端队列，输出到 stderr。
        logger = quill::simple_logger("stderr");
    });
    return logger;
}

}  // namespace

void InitLogging()
{
    (void)LoggerInstance();
}

void Log(LogLevel level, std::string_view category, std::string_view message)
{
    quill::Logger* logger = LoggerInstance();
    switch (level) {
    case LogLevel::Debug:
        LOG_DEBUG(logger, "[{}] {}", category, message);
        break;
    case LogLevel::Info:
        LOG_INFO(logger, "[{}] {}", category, message);
        break;
    case LogLevel::Warning:
        LOG_WARNING(logger, "[{}] {}", category, message);
        break;
    case LogLevel::Error:
        LOG_ERROR(logger, "[{}] {}", category, message);
        break;
    }
}

}  // namespace tcp
