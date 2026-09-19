#include "http/protocol/http2/parser/h2_frame_reader.hpp"
#include <array>
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

static std::array<uint8_t, kFrameHeaderSize> Header(uint32_t length, H2FrameType type)
{
    std::array<uint8_t, kFrameHeaderSize> bytes{};
    EncodeFrameHeader(bytes.data(), {length, type, 0, 1});
    return bytes;
}

static void test_partial_frame_waits_for_payload()
{
    H2FrameReader reader;
    auto header = Header(3, H2FrameType::DATA);
    CHECK(reader.Append(header.data(), header.size()), "写入完整帧头");

    H2FrameReader::Frame frame;
    CHECK(reader.Next(kDefaultMaxFrameSize, frame) == H2FrameReader::NextResult::NeedMore,
          "payload 未到齐时不分发帧");

    const std::array<uint8_t, 3> payload{'a', 'b', 'c'};
    CHECK(reader.Append(payload.data(), payload.size()), "写入 payload");
    CHECK(reader.Next(kDefaultMaxFrameSize, frame) == H2FrameReader::NextResult::Frame,
          "payload 到齐后分发帧");
    CHECK(frame.header.length == 3 && frame.payload[2] == 'c', "帧内容正确");
    reader.Consume(frame);
    CHECK(reader.Buffered() == 0, "消费后缓冲为空");
}

static void test_split_h2c_preface_is_skipped()
{
    H2FrameReader reader;
    const size_t half = kH2ClientPreface.size() / 2;
    CHECK(reader.Append(reinterpret_cast<const uint8_t*>(kH2ClientPreface.data()), half),
          "写入前半段 h2c preface");

    H2FrameReader::Frame frame;
    CHECK(reader.Next(kDefaultMaxFrameSize, frame) == H2FrameReader::NextResult::NeedMore,
          "不完整 h2c preface 等待后续数据");

    auto header = Header(0, H2FrameType::SETTINGS);
    CHECK(reader.Append(reinterpret_cast<const uint8_t*>(kH2ClientPreface.data()) + half,
                        kH2ClientPreface.size() - half), "写入后半段 h2c preface");
    CHECK(reader.Append(header.data(), header.size()), "写入 SETTINGS 帧头");
    CHECK(reader.Next(kDefaultMaxFrameSize, frame) == H2FrameReader::NextResult::Frame,
          "跳过 preface 后读取 SETTINGS");
    CHECK(frame.header.type == H2FrameType::SETTINGS && frame.header.length == 0,
          "跳过 preface 后帧头正确");
}

static void test_oversized_frame_is_reported()
{
    H2FrameReader reader;
    auto header = Header(kDefaultMaxFrameSize + 1, H2FrameType::DATA);
    CHECK(reader.Append(header.data(), header.size()), "写入超长帧头");
    H2FrameReader::Frame frame;
    CHECK(reader.Next(kDefaultMaxFrameSize, frame) == H2FrameReader::NextResult::FrameTooLarge,
          "超长帧在等 payload 前即被拒绝");
}

int main()
{
    test_partial_frame_waits_for_payload();
    test_split_h2c_preface_is_skipped();
    test_oversized_frame_is_reported();
    std::printf("\\nh2_frame_reader_test: PASS=%d FAIL=%d\\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
