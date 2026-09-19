# ═══════════════════════════════════════════════════════════════
# rpc — 基于 protobuf + coro/webcpp-engine 协程的 RPC 框架
#
# 组成：
#   rpc_core      框架核心静态库（帧编解码 / server / client / 流 / 注册表）
#   protoc-gen-rpc protoc 插件（.proto → .rpc.h 桩代码）
#   构建期 protoc 生成：rpc_meta.pb.* / registry.pb.* / greeter.pb.* + .rpc.h
#   registry_server / greeter_server / greeter_client  示例
#   rpc_test / e2e_test                                测试
#
# 依赖：顶层统一构建时复用 coro/webcpp_engine target；独立构建需自行提供。
# ═══════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.20)
project(rpc LANGUAGES CXX)

# ── C++20 + 协程（GCC 需 -fcoroutines；clang 用标准 C++20）──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 依赖检测 ──
# protobuf：macOS homebrew 需显式 CMAKE_PREFIX_PATH（fallback 后报错）
find_package(protobuf CONFIG QUIET)
if(NOT protobuf_FOUND)
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew;/usr/local")
    find_package(protobuf CONFIG REQUIRED)
endif()
find_package(Threads REQUIRED)

# 引擎依赖：顶层统一构建时 target 已存在；独立构建需 -DWEBCPP_ENGINE_DIR
if(NOT TARGET webcpp_engine)
    if(NOT DEFINED WEBCPP_ENGINE_DIR)
        message(FATAL_ERROR "rpc 需通过顶层 CMake 构建（webcpp_engine target），"
                "或 -DWEBCPP_ENGINE_DIR=<webcpp-engine 路径> 独立构建")
    endif()
    add_subdirectory(${WEBCPP_ENGINE_DIR} ${CMAKE_BINARY_DIR}/webcpp-engine)
endif()

# ── 生成目录 ──
set(RPC_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(RPC_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${RPC_GEN_DIR})

# ── 构建期 protoc：生成 .pb.h/.pb.cc（框架元数据 rpc_meta）──
add_custom_command(
    OUTPUT ${RPC_GEN_DIR}/rpc_meta.pb.h ${RPC_GEN_DIR}/rpc_meta.pb.cc
    COMMAND protobuf::protoc
        --proto_path=${RPC_PROTO_DIR}
        --cpp_out=${RPC_GEN_DIR}
        ${RPC_PROTO_DIR}/rpc_meta.proto
    DEPENDS ${RPC_PROTO_DIR}/rpc_meta.proto
    COMMENT "protoc: rpc_meta.pb.*"
    VERBATIM)
add_custom_target(rpc_proto_gen
    DEPENDS ${RPC_GEN_DIR}/rpc_meta.pb.h ${RPC_GEN_DIR}/rpc_meta.pb.cc)

# ── protoc 插件：protoc-gen-rpc（--rpc_out 生成 .rpc.h 桩）──
# 插件实现 protoc 插件协议（stdin 收 CodeGeneratorRequest → stdout 回 Response）
add_executable(protoc-gen-rpc
    plugin/rpc_plugin.cpp
    plugin/codegen.cpp)
target_include_directories(protoc-gen-rpc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin)
target_link_libraries(protoc-gen-rpc PRIVATE protobuf::libprotoc protobuf::libprotobuf)

# ── 构建期 protoc：greeter 示例桩（.pb.* + .rpc.h，经 protoc-gen-rpc 插件）──
set(RPC_EXAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/examples)
add_custom_command(
    OUTPUT ${RPC_GEN_DIR}/greeter.pb.h ${RPC_GEN_DIR}/greeter.pb.cc
           ${RPC_GEN_DIR}/greeter.rpc.h
    COMMAND protobuf::protoc
        --proto_path=${RPC_PROTO_DIR}
        --proto_path=${RPC_EXAMPLE_DIR}
        --cpp_out=${RPC_GEN_DIR}
        --plugin=protoc-gen-rpc=$<TARGET_FILE:protoc-gen-rpc>
        --rpc_out=${RPC_GEN_DIR}
        ${RPC_EXAMPLE_DIR}/greeter.proto
    DEPENDS ${RPC_EXAMPLE_DIR}/greeter.proto protoc-gen-rpc
    COMMENT "protoc: greeter.pb.* + greeter.rpc.h"
    VERBATIM)
add_custom_target(rpc_greeter_gen
    DEPENDS ${RPC_GEN_DIR}/greeter.pb.h ${RPC_GEN_DIR}/greeter.rpc.h)

# ── 构建期 protoc：registry 注册中心（.pb.* + .rpc.h，经 protoc-gen-rpc 插件）──
add_custom_command(
    OUTPUT ${RPC_GEN_DIR}/registry.pb.h ${RPC_GEN_DIR}/registry.pb.cc
           ${RPC_GEN_DIR}/registry.rpc.h
    COMMAND protobuf::protoc
        --proto_path=${RPC_PROTO_DIR}
        --cpp_out=${RPC_GEN_DIR}
        --plugin=protoc-gen-rpc=$<TARGET_FILE:protoc-gen-rpc>
        --rpc_out=${RPC_GEN_DIR}
        ${RPC_PROTO_DIR}/registry.proto
    DEPENDS ${RPC_PROTO_DIR}/registry.proto protoc-gen-rpc
    COMMENT "protoc: registry.pb.* + registry.rpc.h"
    VERBATIM)
add_custom_target(rpc_registry_gen
    DEPENDS ${RPC_GEN_DIR}/registry.pb.h ${RPC_GEN_DIR}/registry.rpc.h)

# ── 框架核心静态库 ──
add_library(rpc_core STATIC
    src/frame.cpp
    src/write_lock.cpp
    src/rpc_stream.cpp
    src/rpc_channel.cpp
    src/rpc_pool.cpp
    src/rpc_server.cpp
    src/rpc_connection.cpp
    src/registry_service.cpp
    src/rpc_client.cpp
    ${RPC_GEN_DIR}/rpc_meta.pb.cc
    ${RPC_GEN_DIR}/registry.pb.cc)
add_dependencies(rpc_core rpc_proto_gen rpc_registry_gen)
target_include_directories(rpc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${RPC_GEN_DIR})
target_link_libraries(rpc_core PUBLIC
    protobuf::libprotobuf
    webcpp_engine
    Threads::Threads)

# ── 单元测试（帧编解码 / 写并发 / 多路复用 / 超时 / 流 / 连接池 / codegen 桩）──
# 链接生成的 greeter.pb.cc：rpc_test 内验证 protoc 插件桩的往返
add_executable(rpc_test test/rpc_test.cpp ${RPC_GEN_DIR}/greeter.pb.cc)
add_dependencies(rpc_test rpc_greeter_gen)
target_link_libraries(rpc_test PRIVATE rpc_core)

# ── 示例：greeter 直连模式（P7 起接入 registry 服务发现）──
add_executable(greeter_server examples/greeter_server.cpp ${RPC_GEN_DIR}/greeter.pb.cc)
add_dependencies(greeter_server rpc_greeter_gen)
target_link_libraries(greeter_server PRIVATE rpc_core)

add_executable(greeter_client examples/greeter_client.cpp ${RPC_GEN_DIR}/greeter.pb.cc)
add_dependencies(greeter_client rpc_greeter_gen)
target_link_libraries(greeter_client PRIVATE rpc_core)

# ── 示例：独立 registry 注册中心（P7）──
add_executable(registry_server examples/registry_server.cpp ${RPC_GEN_DIR}/registry.pb.cc)
add_dependencies(registry_server rpc_registry_gen)
target_link_libraries(registry_server PRIVATE rpc_core)

# ── 端到端测试（registry + greeter server + client，三线程三个 EventLoop）──
add_executable(e2e_test test/e2e_test.cpp ${RPC_GEN_DIR}/greeter.pb.cc)
add_dependencies(e2e_test rpc_greeter_gen)
target_link_libraries(e2e_test PRIVATE rpc_core)
