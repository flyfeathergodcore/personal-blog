cmake_minimum_required(VERSION 3.20)
project(webcpp_engine LANGUAGES CXX C)

include(CTest)

# ═══════════════════════════════════════════════════════════════
# webcpp_engine — TCP 传输层与 HTTP 服务层
#
# 从 webcpp 分离出的可复用核心：
#   webcpp_tcp   操作系统 TCP socket、收发、监听与连接原语
#   webcpp_net   TLS、信号等通用网络组件，依赖 webcpp_tcp
#   tcp/         基于 webcpp_tcp 组装的原始 TCP 服务器
#   http/        HTTP 服务、协议、路由、缓存、配置与日志
#
# 外部依赖：coro（独立协程库）、OpenSSL、yaml-cpp、nghttp2、Threads、dl。
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

# ── coro 协程运行时（随 webcpp-engine 分发）──
set(CORO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/coro" CACHE PATH "coro 协程库路径")
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
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

# ── Quill asynchronous logging backend ──
if(NOT TARGET quill::quill)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/quill
        ${CMAKE_CURRENT_BINARY_DIR}/quill EXCLUDE_FROM_ALL)
endif()

# ── High-concurrency asynchronous logging ──
add_library(webcpp_log INTERFACE)
target_include_directories(webcpp_log INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webcpp_log INTERFACE quill::quill)

# ── TCP transport layer ──
set(WEBCPP_TCP_SOURCES
    tcp/tcp_log.cpp
    tcp/stream.cpp
    tcp/listener.cpp
    tcp/connector.cpp
)

add_library(webcpp_tcp STATIC ${WEBCPP_TCP_SOURCES})

target_include_directories(webcpp_tcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(webcpp_tcp PUBLIC Threads::Threads dl coro webcpp_log)

# ── Generic networking components ──
set(WEBCPP_NET_SOURCES
    net/signal_watcher.cpp
    net/tls_context.cpp
    net/tls_stream.cpp
)

add_library(webcpp_net STATIC ${WEBCPP_NET_SOURCES})

target_include_directories(webcpp_net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(webcpp_net PUBLIC
    webcpp_tcp OpenSSL::SSL OpenSSL::Crypto)

# ── TCP service layer ──
add_library(webcpp_tcp_service STATIC
    tcp/accept_strategy.cpp
    tcp/reactor.cpp
    tcp/channel.cpp
    tcp/server.cpp)

target_include_directories(webcpp_tcp_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(webcpp_tcp_service PUBLIC webcpp_tcp webcpp_net)

# ── HTTP service layer ──
set(WEBCPP_HTTP_SOURCES
    http/config/config.cpp
    http/server/server.cpp
    http/server/multi_server.cpp
    http/server/sse_push.cpp
    http/server/h11_session.cpp
    http/server/h11_session_pool.cpp
    http/server/h2_session.cpp
    http/server/h2_stream_writer.cpp
    http/server/h2_stream_processor.cpp
    http/server/h2_ws_stream.cpp
    http/protocol/response.cpp
    http/protocol/region_pool.cpp
    http/protocol/session_region.cpp
    http/protocol/ws_frame.cpp
    http/protocol/protocol.cpp
    http/protocol/http1.1/parser.cpp
    http/protocol/http2/stream_context.cpp
    http/protocol/http2/parser/HPACK.cpp
    http/protocol/http2/parser/h2_frame_encoder.cpp
    http/protocol/http2/parser/h2_frame_reader.cpp
    http/protocol/http2/parser/stream_manager.cpp
    http/protocol/http2/parser/flow_control.cpp
    http/middleware/middleware.cpp
    http/handler/metrics.cpp
    http/handler/upstream_pool.cpp
    http/handler/reverse_proxy.cpp
    http/handler/proxy_handler.cpp
    http/handler/request_handler.cpp
    http/handler/health.cpp
    http/handler/loader.cpp
    http/handler/reloader.cpp
    http/server/upstream_conn_pool.cpp
    http/router/router.cpp
    http/cache/file_cache.cpp
)

add_library(webcpp_http STATIC ${WEBCPP_HTTP_SOURCES})

target_include_directories(webcpp_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(webcpp_http PUBLIC
    webcpp_tcp webcpp_net webcpp_log yaml-cpp)

# 保留旧目标，避免既有工程从 webcpp_engine 迁移时发生链接中断。
add_library(webcpp_engine INTERFACE)
target_link_libraries(webcpp_engine INTERFACE webcpp_http)

# ── TCP 层单元测试 ──
add_executable(net_test test/net_test.cpp)
target_include_directories(net_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(net_test PRIVATE webcpp_tcp webcpp_net)
# GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(net_test PRIVATE -fcoroutines)
endif()

# ── Muduo 风格 EventLoopThreadPool 测试 ──
add_executable(event_loop_thread_pool_test test/event_loop_thread_pool_test.cpp)
target_include_directories(event_loop_thread_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(event_loop_thread_pool_test PRIVATE coro Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(event_loop_thread_pool_test PRIVATE -fcoroutines)
endif()
add_test(NAME event_loop_thread_pool_test COMMAND event_loop_thread_pool_test)

# ── 日志测试（Logger::Init/Log/StopAll + 级别过滤 + 落盘）──
add_executable(log_test test/log_test.cpp)
target_include_directories(log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(log_test PRIVATE Threads::Threads webcpp_log)
# GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(log_test PRIVATE -fcoroutines)
endif()

# ── HTTP 服务（H1/SSE/WS + TLS）──
add_executable(demo_server http/http_server.cpp)
target_include_directories(demo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(demo_server PRIVATE webcpp_engine)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
    target_compile_options(demo_server PRIVATE -fcoroutines)
endif()

# ── TCP 测试服务：echo / 简化 HTTP keep-alive 压测 ──
add_executable(tcp_server
    test/tcp/tcp_server.cpp
    test/tcp/echo_handler.cpp
    test/tcp/http_bench_handler.cpp)
target_include_directories(tcp_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcp_server PRIVATE webcpp_tcp_service)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(tcp_server PRIVATE -fcoroutines)
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
pkg_check_modules(MARIADB QUIET libmariadb)
if(MARIADB_FOUND)
    set(MYSQL_INCLUDE_DIR ${MARIADB_INCLUDE_DIRS})
    set(MYSQL_LIBRARY ${MARIADB_LIBRARIES})
    set(MYSQL_IS_MARIADB ON)
endif()
if(NOT MYSQL_IS_MARIADB)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h)
    find_library(MYSQL_LIBRARY NAMES mysqlclient libmysqlclient)
endif()

set(MYSQL_POOL_DIR "" CACHE PATH "mysql_connection_pool 源码目录")
if(TARGET mysql_connection_pool)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        target_sources(demo_server PRIVATE http/handler/mysql_handler.cpp)
        target_include_directories(demo_server PRIVATE ${MYSQL_INCLUDE_DIR})
        target_link_libraries(demo_server PRIVATE mysql_connection_pool ${MYSQL_LIBRARY})
        message(STATUS "MySQL 博客接口已启用 (链接 mysql_connection_pool target)")
    else()
        message(WARNING "未找到 MySQL client 库，博客接口将不可用")
    endif()
elseif(MYSQL_POOL_DIR)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        target_sources(demo_server PRIVATE
            http/handler/mysql_handler.cpp
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
add_executable(routes_demo http/routes_demo.cpp)
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

# ── H2 流控账本单元测试（纯逻辑：不含 coro / socket / TLS）──
add_executable(flow_control_test test/flow_control_test.cpp
    http/protocol/http2/parser/flow_control.cpp)
target_include_directories(flow_control_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(stream_manager_test test/stream_manager_test.cpp
    http/protocol/http2/parser/stream_manager.cpp)
target_include_directories(stream_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(h2_frame_reader_test test/h2_frame_reader_test.cpp)
target_include_directories(h2_frame_reader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(h2_frame_reader_test PRIVATE webcpp_engine)

# ── HTTP/2 端到端测试：进程内 TLS 服务端 + nghttp2 客户端 ──
add_executable(h2_e2e_test test/h2_e2e_test.cpp)
target_include_directories(h2_e2e_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(h2_e2e_test PRIVATE webcpp_engine PkgConfig::NGHTTP2)
enable_testing()
if(BUILD_TESTING)
    add_test(NAME net_test COMMAND net_test)
    add_test(NAME log_test COMMAND log_test)
    add_test(NAME router_test COMMAND router_test)
    add_test(NAME flow_control_test COMMAND flow_control_test)
    add_test(NAME stream_manager_test COMMAND stream_manager_test)
    add_test(NAME h2_frame_reader_test COMMAND h2_frame_reader_test)
    add_test(NAME h2_e2e_test COMMAND h2_e2e_test)
    add_test(NAME demo_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/demo_smoke.sh
            $<TARGET_FILE:demo_server> 18443
            ${CMAKE_CURRENT_SOURCE_DIR}/test/certs/server.crt
            ${CMAKE_CURRENT_SOURCE_DIR}/test/certs/server.key)
    set_tests_properties(demo_smoke PROPERTIES TIMEOUT 60)
    add_test(NAME tcp_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/tcp_smoke.sh
            $<TARGET_FILE:tcp_server> 18082)
    set_tests_properties(tcp_smoke PROPERTIES TIMEOUT 20)
    add_test(NAME tcp_smoke_single
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/tcp_smoke.sh
            $<TARGET_FILE:tcp_server> 18083 -a single)
    set_tests_properties(tcp_smoke_single PROPERTIES TIMEOUT 20)
    add_test(NAME tcp_http_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/tcp_http_smoke.sh
            $<TARGET_FILE:tcp_server> 18084)
    set_tests_properties(tcp_http_smoke PROPERTIES TIMEOUT 20)
    add_test(NAME tcp_http_smoke_single
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/tcp_http_smoke.sh
            $<TARGET_FILE:tcp_server> 18085 -a single)
    set_tests_properties(tcp_http_smoke_single PROPERTIES TIMEOUT 20)
endif()

# ── 可安装（便于未来 find_package 独立消费）──
include(GNUInstallDirs)
install(TARGETS webcpp_tcp webcpp_net webcpp_tcp_service webcpp_http
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY log net tcp http
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/webcpp_engine
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
