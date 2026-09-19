#pragma once

#include <string_view>

namespace tcp {

// TCP 层的日志级别，避免业务代码直接依赖 Quill 的枚举类型。
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

/*
1. 初始化 TCP 日志后端；函数幂等，可由服务启动流程显式调用
2. 日志通过 Quill 的前端队列异步提交，调用线程不直接写控制台
3. category 用于区分 TCP 子模块，message 为已经准备好的日志正文
*/
void InitLogging();
void Log(LogLevel level, std::string_view category, std::string_view message);

}  // namespace tcp
