# 独立协程库：零依赖，C++20
cmake_minimum_required(VERSION 3.10)
project(coro LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(coro STATIC
    src/frame_pool.cpp
    src/event_loop.cpp
)
target_include_directories(coro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(coro PUBLIC cxx_std_20)

if(APPLE)
    target_sources(coro PRIVATE src/io_poller_kqueue.cpp)
elseif(UNIX)
    target_sources(coro PRIVATE src/io_poller_epoll.cpp)
endif()
