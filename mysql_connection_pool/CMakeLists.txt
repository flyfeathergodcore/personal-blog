# 基础设置 cmake基础版本、项目名称、C++标准、构建类型
cmake_minimum_required(VERSION 3.10)
project(mysql_connection_pool)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# 生成 compile_commands.json，给 VS Code / clangd 提供 IntelliSense 信息
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 协程库统一使用外部 coro（不再内置副本，消除两份 coro 分叉 / ODR 风险）：
# 默认取仓库根目录顶层 coro（即本文件上一级），可用 -DCORO_DIR=... 覆盖。
set(CORO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../coro" CACHE PATH "外部 coro 独立协程库路径")
# 作为顶层统一构建的子目录时 coro 已由父工程添加，跳过（避免 target 重定义）
if(NOT TARGET coro)
    if(NOT EXISTS ${CORO_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "coro 库未找到: ${CORO_DIR}（用 -DCORO_DIR=... 指定）")
    endif()
    add_subdirectory(${CORO_DIR} ${CMAKE_BINARY_DIR}/coro)
endif()

# 查找 MySQL 头文件路径
# 约定: 代码中使用 #include <mysql/mysql.h>，所以需要找到 mysql/ 的父目录
find_path(MYSQL_INCLUDE_DIR
    NAMES mysql/mysql.h
    PATHS
        /opt/homebrew/opt/mysql-client/include
        /opt/homebrew/include
        /usr/local/opt/mysql-client/include
        /usr/local/include
        /usr/include
)
# 如果上面没找到，用 mysql_config 自动检测
if(NOT MYSQL_INCLUDE_DIR OR NOT EXISTS "${MYSQL_INCLUDE_DIR}/mysql/mysql.h")
    find_program(MYSQL_CONFIG mysql_config)
    if(MYSQL_CONFIG)
        execute_process(
            COMMAND ${MYSQL_CONFIG} --include
            OUTPUT_VARIABLE MYSQL_INCLUDE_FLAGS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REGEX REPLACE "^-I" "" MYSQL_INCLUDE_RAW "${MYSQL_INCLUDE_FLAGS}")
        # mysql_config 返回的可能已经是 mysql/ 子目录（如 /opt/homebrew/.../include/mysql），
        # 但代码需要 #include <mysql/mysql.h>，这里取父目录
        get_filename_component(MYSQL_INCLUDE_DIR "${MYSQL_INCLUDE_RAW}" DIRECTORY)
    endif()
endif()

# 查找 MySQL 库文件
find_library(MYSQL_LIBRARY
    NAMES mysqlclient libmysqlclient
    PATHS
        /opt/homebrew/opt/mysql-client/lib
        /opt/homebrew/lib
        /usr/local/opt/mysql-client/lib
        /usr/local/lib
)

if(NOT EXISTS ${MYSQL_INCLUDE_DIR} OR NOT EXISTS ${MYSQL_LIBRARY})
    message(FATAL_ERROR
        "\n========================================\n"
        "MySQL client library not found!\n"
        "Expected:\n"
        "  Include: ${MYSQL_INCLUDE_DIR}\n"
        "  Library: ${MYSQL_LIBRARY}\n"
        "========================================\n"
    )
endif()

# 测试子目录：test/CMakeLists.txt 使用 ${MYSQL_INCLUDE_DIR} / ${MYSQL_LIBRARY}，
# 故 MySQL 查找块必须位于 add_subdirectory(test) 之前，否则全新配置首次构建
# 时 test_async 缺 MySQL 头路径（find_path 的 cache 值要等本块执行后才写入）
add_subdirectory(test)

# 设置编译类型、编译选项、链接选项等
set(SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/connectionpool.cpp
)
# 必须用 STATIC：coro 是静态库，若本目标为 SHARED，coro 代码（含
# EventLoop::current 的 thread_local g_current_loop）会分别嵌入 dylib 和
# 最终可执行文件，产生两份独立 TLS——connection.cpp（dylib 内）调
# EventLoop::current() 时惰性新建"影子 EventLoop"，异步协程全部挂到
# 影子实例上，主循环的定时器/IO 永不恢复它们。STATIC 后符号并入
# 可执行文件，TLS 单例。
add_library(mysql_connection_pool STATIC ${SOURCE_FILES})

target_include_directories(mysql_connection_pool
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src  # 如果有内部头文件
)

# 异步接口（async_connect/async_query/async_update）依赖 coro 协程库：
# 帧分配走 FramePool、IO 等待走 EventLoop，必须链接 coro（同时获得其 PUBLIC include 路径）
target_link_libraries(mysql_connection_pool PRIVATE coro ${MYSQL_LIBRARY})
target_include_directories(mysql_connection_pool PRIVATE ${MYSQL_INCLUDE_DIR})
