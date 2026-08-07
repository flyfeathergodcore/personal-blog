# 协程库单元测试（零测试框架依赖）
add_executable(test_coro test_coro.cpp)
target_link_libraries(test_coro PRIVATE coro)

# 异步连接/连接池测试（失败路径始终执行；集成测试需 MYSQL_TEST_* 环境变量）
# test_pool.cpp 由 Task 3 填充（池测试），本任务先创建占位文件
add_executable(test_async test_async.cpp test_pool.cpp
    test_mysql.cpp)
target_include_directories(test_async PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${MYSQL_INCLUDE_DIR}
)
target_link_libraries(test_async PRIVATE coro mysql_connection_pool ${MYSQL_LIBRARY})
target_compile_features(test_async PRIVATE cxx_std_20)
