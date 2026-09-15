// H2 流控账本单元测试：纯逻辑，无 socket / TLS / 协程
#include "protocol/http2/parser/flow_control.hpp"
#include <cstdint>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { ++g_pass; }                                              \
        else {                                                                 \
            ++g_fail;                                                         \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);        \
        }                                                                      \
    } while (0)

// ── 接收账：连接窗口不被重复扣减 ──
// B1：旧调用点连续调两次 ConsumeBytes（一次流一次连接），而 ConsumeBytes
// 内部已扣连接账。契约定为「调用方只调一次，连接账在内部扣」。
static void test_consume_recv_deducts_connection_once()
{
    H2FlowControl fc;                       // 初始窗口 65535
    fc.ConsumeRecv(1, 1000);

    // 流与连接各扣 1000，而不是连接扣 2000
    CHECK(fc.RecvWindow(1) == 64535, "流级接收窗口扣 1000");
    CHECK(fc.RecvWindow(0) == 64535, "连接级接收窗口只扣一次 1000");
}

// ── 接收账：SETTINGS 不动连接窗口（RFC 7540 §6.9.2）──
// B19：旧的 SetInitialWindow 会一起调整 conn_.credit。
//
// 注意 RecvWindow 取【流与连接的较小值】，而 SETTINGS 只放大流级窗口时连接
// 必然仍是瓶颈——调大的方向看不出流级的变化。所以要观测流级的 delta，
// 必须用【调小】把流级压成瓶颈；调大的方向则单独验证连接窗口没被带上。
static void test_local_settings_do_not_touch_connection_window()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 100);                 // 流 1 与连接各扣 100

    fc.SetLocalInitialWindow(32768);        // 调小：流级成为瓶颈，变化可见
    CHECK(fc.RecvWindow(1) == 32768 - 100,
          "流级接收窗口按 delta(-32767) 调整");
    CHECK(fc.RecvWindow(0) == 65535 - 100,
          "连接级接收窗口不受 SETTINGS 影响");

    // 反向验证：调大时若像旧实现那样把 delta 也加到连接账，
    // 连接窗口会变成 130972 —— 这里必须是 65435。
    H2FlowControl fc2;
    fc2.ConsumeRecv(1, 100);
    fc2.SetLocalInitialWindow(131072);
    CHECK(fc2.RecvWindow(0) == 65535 - 100,
          "调大时连接级窗口同样不受影响（旧实现会变成 130972）");
}

// ── 接收账：补窗口的增量等于已消费量 ──
static void test_pop_credit_returns_consumed_amount()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 40000);               // ≥ 初始窗口一半（32767）

    CHECK(fc.ShouldUpdate(1), "消费过半应补窗口");
    CHECK(fc.ShouldUpdate(0), "连接级消费过半同样应补");

    uint32_t s_credit = fc.PopCredit(1);
    uint32_t c_credit = fc.PopCredit(0);
    CHECK(s_credit == 40000, "流级 WINDOW_UPDATE 增量 = 已消费量");
    CHECK(c_credit == 40000, "连接级 WINDOW_UPDATE 增量 = 已消费量");

    CHECK(!fc.ShouldUpdate(1), "补过之后不再重复补");
    CHECK(fc.RecvWindow(1) == 65535, "补窗口后流级恢复到初始值");
}

// ── 接收账：未消费过半时不补 ──
static void test_should_update_below_threshold()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 100);
    CHECK(!fc.ShouldUpdate(1), "消费未过半不补窗口");
    CHECK(!fc.ShouldUpdate(0), "连接级同上");
}

int main()
{
    test_consume_recv_deducts_connection_once();
    test_local_settings_do_not_touch_connection_window();
    test_pop_credit_returns_consumed_amount();
    test_should_update_below_threshold();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
