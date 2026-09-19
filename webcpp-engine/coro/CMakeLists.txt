# 独立协程库：零依赖，C++20
cmake_minimum_required(VERSION 3.10)
project(coro LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MIMALLOC QUIET mimalloc)
endif()
find_path(MIMALLOC_HEADER_DIR NAMES mimalloc.h
    HINTS ${MIMALLOC_INCLUDEDIR} ${MIMALLOC_INCLUDE_DIRS}
    PATH_SUFFIXES mimalloc-3.5 mimalloc-3.0 mimalloc-2.1 mimalloc-2.0)
if(NOT MIMALLOC_HEADER_DIR)
    message(FATAL_ERROR "mimalloc.h was not found; install the mimalloc development package")
endif()
find_library(MIMALLOC_LIBRARY NAMES mimalloc libmimalloc
    HINTS ${MIMALLOC_LIBDIR} ${MIMALLOC_LIBRARY_DIRS}
    PATH_SUFFIXES mimalloc-3.5 mimalloc-3.0 mimalloc-2.1 mimalloc-2.0)
if(NOT MIMALLOC_LIBRARY)
    message(FATAL_ERROR "libmimalloc was not found; install the mimalloc development package")
endif()

add_library(coro STATIC
    include/coro/io_poller_epoll.h
    include/coro/io_poller_kqueue.h
    src/frame_pool.cpp
    src/event_loop.cpp
    src/event_loop_thread.cpp
    src/event_loop_thread_pool.cpp
)
target_include_directories(coro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(coro PRIVATE ${MIMALLOC_HEADER_DIR})
target_compile_features(coro PUBLIC cxx_std_20)
target_link_libraries(coro PUBLIC ${MIMALLOC_LIBRARY})

if(APPLE)
    target_sources(coro PRIVATE src/io_poller_kqueue.cpp)
elseif(UNIX)
    target_sources(coro PRIVATE src/io_poller_epoll.cpp)
endif()
