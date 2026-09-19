#include "http/protocol/http2/parser/stream_manager.hpp"
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else {                                                                 \
            ++g_fail;                                                         \
            std::printf("FAIL: %s (%s:%d)\\n", msg, __FILE__, __LINE__);     \
        }                                                                      \
    } while (0)

static void test_invalid_client_ids_are_protocol_errors()
{
    H2StreamManager streams;
    CHECK(streams.OnStreamOpen(2) == H2StreamManager::OpenResult::ProtocolError,
          "偶数客户端流 ID 是连接级协议错误");
    CHECK(streams.OnStreamOpen(1) == H2StreamManager::OpenResult::Accepted,
          "首个奇数客户端流 ID 可创建");
    CHECK(streams.OnStreamOpen(1) == H2StreamManager::OpenResult::ProtocolError,
          "重复流 ID 是连接级协议错误");
}

static void test_concurrency_limit_is_stream_refusal()
{
    H2StreamManager streams;
    streams.SetMaxConcurrent(1);
    CHECK(streams.OnStreamOpen(1) == H2StreamManager::OpenResult::Accepted,
          "上限内的流可创建");
    CHECK(streams.OnStreamOpen(3) == H2StreamManager::OpenResult::Refused,
          "超并发上限只拒绝该流");
    streams.RemoveStream(1);
    CHECK(streams.OnStreamOpen(3) == H2StreamManager::OpenResult::Accepted,
          "已有流关闭后可继续创建新流");
}

int main()
{
    test_invalid_client_ids_are_protocol_errors();
    test_concurrency_limit_is_stream_refusal();
    std::printf("\\nstream_manager_test: PASS=%d FAIL=%d\\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
