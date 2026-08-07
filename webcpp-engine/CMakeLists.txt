cmake_minimum_required(VERSION 3.20)
project(webcpp_engine LANGUAGES CXX C)

# ═══════════════════════════════════════════════════════════════
# webcpp_engine — 独立 HTTP 网络引擎（静态库）
#
# 从 webcpp 分离出的可复用 HTTP 核心：
#   net/        网络 I/O 原语（tcp/tls stream / listener / resolver / buffered_reader）
#   server/     服务器与会话（server / multi_server / h11_session / h2_session / ws）
#   protocol/   HTTP 协议层（http1.1 / http2 手写解析器、response、session_region）
#   router/     路由
#   middleware/ 中间件链（洋葱模型）
#   handler/    框架级 handler（metrics / health / static_file / proxy）
#   cache/      文件缓存
#   config/     YAML 配置加载
#   log/        结构化日志
#
# 外部依赖：coro（独立协程库）、OpenSSL、yaml-cpp、Threads、dl
# 无 grpc / sqlite / nghttp2 / asio 依赖。
# ═══════════════════════════════════════════════════════════════

# ── Release build with optimizations ──
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── C++20 Standard + Coroutines ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── coro 独立协程库 ──
set(CORO_DIR "/home/ubuntu/coro" CACHE PATH "coro 独立协程库路径")
# 作为顶层统一构建的子目录时，coro 已由父工程 add_subdirectory，此处跳过
# （重复 add 会 target 重定义）；独立构建时自动从 CORO_DIR 引入
if(NOT TARGET coro)
    if(NOT EXISTS ${CORO_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "coro 库未找到: ${CORO_DIR}")
    endif()
    add_subdirectory(${CORO_DIR} ${CMAKE_BINARY_DIR}/coro)
endif()

# ── Dependencies ──
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(OpenSSL REQUIRED)

# ── Engine source list ──
set(WEBCPP_ENGINE_SOURCES
    config/config.cpp
    server/server.cpp
    server/multi_server.cpp
    server/sse_push.cpp
    server/h11_session.cpp
    server/h11_session_pool.cpp
    server/h2_session.cpp
    protocol/response.cpp
    protocol/region_pool.cpp
    protocol/session_region.cpp
    protocol/ws_frame.cpp
    protocol/protocol.cpp
    protocol/http1.1/parser.cpp
    protocol/http2/stream_context.cpp
    protocol/http2/parser/HPACK.cpp
    protocol/http2/parser/stream_manager.cpp
    protocol/http2/parser/flow_control.cpp
    net/tcp_stream.cpp
    net/tcp_listener.cpp
    net/resolver.cpp
    net/buffered_reader.cpp
    net/signal_watcher.cpp
    net/tls_context.cpp
    net/tls_stream.cpp
    middleware/middleware.cpp
    src/handler/metrics.cpp
    src/handler/upstream_pool.cpp
    src/handler/reverse_proxy.cpp
    src/handler/proxy_handler.cpp
    src/handler/request_handler.cpp
    src/handler/health.cpp
    src/handler/loader.cpp
    src/handler/reloader.cpp
    server/upstream_conn_pool.cpp
    router/router.cpp
    cache/file_cache.cpp
)

add_library(webcpp_engine STATIC ${WEBCPP_ENGINE_SOURCES})

# ── net 层单元测试 ──
add_executable(net_test test/net_test.cpp net/tcp_stream.cpp
    net/tcp_listener.cpp net/resolver.cpp
    net/buffered_reader.cpp net/signal_watcher.cpp
    net/tls_context.cpp net/tls_stream.cpp)
target_include_directories(net_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(net_test PRIVATE coro OpenSSL::SSL OpenSSL::Crypto)
# GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(net_test PRIVATE -fcoroutines)
endif()

# ── 日志测试（Logger::Init/Log/StopAll + 级别过滤 + 落盘）──
add_executable(log_test test/log_test.cpp)
target_include_directories(log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(log_test PRIVATE Threads::Threads)
# GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(log_test PRIVATE -fcoroutines)
endif()

# ── 演示服务器（H1/SSE/WS + TLS，基于 coro net 原语）──
add_executable(demo_server examples/demo_server.cpp)
target_include_directories(demo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(demo_server PRIVATE webcpp_engine)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
    target_compile_options(demo_server PRIVATE -fcoroutines)
endif()

# ── 博客后端 MySQL 集成（连接池 + handler 并入 demo_server，静态链接）──
# 两种模式：
#   统一构建（顶层 add_subdirectory(mysql_connection_pool)）→ 链接连接池静态库
#     target，其 PUBLIC include 自动提供 connectionpool.h 的解析路径；
#   独立构建 → 传 -DMYSQL_POOL_DIR=... 把连接池源码直接编入 demo_server。
# mysqlclient 提供 MySQL 8.0+ 非阻塞 API；coro 须与引擎共用同一份，避免
# EventLoop TLS 双实例。
# ── MySQL 客户端库检测：优先 mariadb-connector-c（Alpine 等无官方 libmysqlclient）──
# 与 mysql_connection_pool/CMakeLists.txt 同一策略：pkg-config 找到 libmariadb 则用
# mariadb（MYSQL_IS_MARIADB=ON），否则 fallback 官方 libmysqlclient。链接 target
# 时宏由 mysql_connection_pool 的 PUBLIC 定义传递；源码编入（MYSQL_POOL_DIR）模式
# 需在本块显式补 MARIADB_CONNECTOR_C/MARIADB_ASYNC（后者须在 #include <mysql/mysql.h>
# 前生效，使 mysql_*_start/cont 声明可见）。
set(MYSQL_IS_MARIADB OFF)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MARIADB QUIET libmariadb)
    if(MARIADB_FOUND)
        set(MYSQL_INCLUDE_DIR ${MARIADB_INCLUDE_DIRS})
        set(MYSQL_LIBRARY ${MARIADB_LIBRARIES})
        set(MYSQL_IS_MARIADB ON)
    endif()
endif()
if(NOT MYSQL_IS_MARIADB)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h)
    find_library(MYSQL_LIBRARY NAMES mysqlclient libmysqlclient)
endif()

set(MYSQL_POOL_DIR "" CACHE PATH "mysql_connection_pool 源码目录")
if(TARGET mysql_connection_pool)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        target_sources(demo_server PRIVATE src/handler/mysql_handler.cpp)
        target_include_directories(demo_server PRIVATE ${MYSQL_INCLUDE_DIR})
        target_link_libraries(demo_server PRIVATE mysql_connection_pool ${MYSQL_LIBRARY})
        message(STATUS "MySQL 博客接口已启用 (链接 mysql_connection_pool target)")
    else()
        message(WARNING "未找到 MySQL client 库，博客接口将不可用")
    endif()
elseif(MYSQL_POOL_DIR)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        target_sources(demo_server PRIVATE
            src/handler/mysql_handler.cpp
            ${MYSQL_POOL_DIR}/src/connection.cpp
            ${MYSQL_POOL_DIR}/src/connectionpool.cpp)
        target_include_directories(demo_server PRIVATE
            ${MYSQL_POOL_DIR}/include ${MYSQL_POOL_DIR}/src ${MYSQL_INCLUDE_DIR})
        target_link_libraries(demo_server PRIVATE ${MYSQL_LIBRARY})
        if(MYSQL_IS_MARIADB)
            target_compile_definitions(demo_server PRIVATE MARIADB_CONNECTOR_C MARIADB_ASYNC)
        endif()
        message(STATUS "MySQL 博客接口已启用 (MYSQL_POOL_DIR=${MYSQL_POOL_DIR})")
    else()
        message(WARNING "未找到 MySQL client 库，博客接口将不可用")
    endif()
endif()

# ── 路由/处理器接线示例（演示如何添加路由与处理逻辑）──
add_executable(routes_demo examples/routes_demo.cpp)
target_include_directories(routes_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(routes_demo PRIVATE webcpp_engine)
# GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(routes_demo PRIVATE -fcoroutines)
endif()

# ── 路由单元测试（query 剥离 / 参数捕获 / lambda 注册）──
add_executable(router_test test/router_test.cpp)
target_include_directories(router_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(router_test PRIVATE webcpp_engine)
# GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(router_test PRIVATE -fcoroutines)
endif()

# 引擎根目录 + include/ 作为 include 根，使消费方可用 #include "net/..." /
# #include "handler/..." 等相对路径（内置 handler 头文件已迁入 include/）。
# 依赖以 PUBLIC 传递，应用链接 webcpp_engine 即获得 OpenSSL/yaml-cpp/coro。
target_include_directories(webcpp_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(webcpp_engine PUBLIC
    Threads::Threads yaml-cpp
    OpenSSL::SSL OpenSSL::Crypto dl coro)

# ── 可安装（便于未来 find_package 独立消费）──
include(GNUInstallDirs)
install(TARGETS webcpp_engine
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY net server protocol router middleware cache config log
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/webcpp_engine
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(DIRECTORY include/handler
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/webcpp_engine
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
