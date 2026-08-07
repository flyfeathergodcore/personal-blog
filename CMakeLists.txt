# ═══════════════════════════════════════════════════════════════════
# personal-blog 统一构建入口（macOS / Linux / Docker 通用）
#
# 一次性构建三个子工程：
#   coro/                 独立 C++20 协程库（epoll/kqueue 事件循环）
#   mysql_connection_pool/ MySQL 8.0+ 异步连接池（依赖外部 coro）
#   webcpp-engine/         HTTP/1.1 + HTTP/2 引擎（依赖 coro + OpenSSL + yaml-cpp）
#
# 用法：
#   cmake -S . -B build && cmake --build build -j4        # 构建全部
#   cmake --build build --target demo_server               # 只构建后端服务器
#   前端 my-web 不经 CMake：cd my-web && npm run build
#
# 可选参数：
#   -DWEBCPP_MYSQL=OFF   跳过 MySQL 连接池与博客接口（引擎仍可编译）
#   macOS 首次构建需：brew install yaml-cpp openssl mysql-client
# ═══════════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.20)
project(personal_blog LANGUAGES CXX C)

# ── Release 默认 ──
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── C++20 标准 + GCC 协程参数 ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # GCC 需 -fcoroutines 才启用协程；clang 用标准 C++20 协程，无需且不支持该参数
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 子项目 ──
option(WEBCPP_MYSQL "构建 MySQL 连接池与博客接口" ON)
add_subdirectory(coro)
if(WEBCPP_MYSQL)
    add_subdirectory(mysql_connection_pool)
endif()
add_subdirectory(webcpp-engine)

message(STATUS "前端 my-web 不经 CMake 构建：cd my-web && npm run build")
