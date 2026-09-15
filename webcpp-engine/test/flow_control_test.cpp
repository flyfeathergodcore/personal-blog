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

    // 反向验证两件事：
    // (1) 调大时若像旧实现那样把 delta 也加到连接账，连接窗口会变成 130972
    //     —— 这里必须是 65435。
    // (2) 此刻流级(130972) > 连接级(65435)，RecvWindow(1) 必须取小的那个。
    //     这是【唯一】能证明 RecvWindow 真的做了 min 的场景：调小的方向里
    //     流级本来就在连接级下面，取不取 min 数值相同，分辨不出来。
    H2FlowControl fc2;
    fc2.ConsumeRecv(1, 100);
    fc2.SetLocalInitialWindow(131072);
    CHECK(fc2.RecvWindow(0) == 65535 - 100,
          "调大时连接级窗口同样不受影响（旧实现会变成 130972）");
    CHECK(fc2.RecvWindow(1) == 65535 - 100,
          "流级(130972) > 连接级(65435) 时取较小值——漏了 min 这里会给出 130972");
}

// ── 接收账：连接级补窗口阈值只跟连接窗口有关 ──
// 连接窗口不受 SETTINGS_INITIAL_WINDOW_SIZE 影响（§6.9.2），阈值必须用连接
// 自己的初始值。若误用流的 recv_initial_：本端把接收窗口广告成 10MB 后阈值
// 变成 5MB，而连接 consumed 受 65535 上限约束永远够不到 → ShouldUpdate(0)
// 永假 → 连接窗口耗尽后再不补充 → 整条连接停滞。
// （与 h2_session.cpp 里记录的「peer 10MB → 大 body 死锁」同源。）
static void test_conn_should_update_uses_connection_initial()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 40000);                    // 连接 consumed = 40000 ≥ 32767

    fc.SetLocalInitialWindow(10 * 1024 * 1024);  // 本端广告 10MB 流级窗口
    CHECK(fc.ShouldUpdate(0),
          "连接级阈值仍是 65535/2，不该被流级初始窗口带高");
}

// ── 接收账：未知流也要受连接窗口约束 ──
// RecvWindow 用 find 不建条目，所以首个 DATA 到达、账目尚未创建时就会被问到
// 未知流。此时流级按初始窗口满额，但仍不能突破连接窗口（与 SendWindow 对称）。
static void test_recv_window_unknown_stream_respects_connection()
{
    H2FlowControl fc;
    fc.ConsumeRecv(0, 65000);                    // 只动连接账：剩 535
    CHECK(fc.RecvWindow(99) == 535,
          "未知流的流级是满额 65535，但必须被连接窗口压到 535");
}

// ── 接收账：窗口被压成负数时对外表现为 0 ──
// RFC 7540 §6.9.2 允许 SETTINGS 把流级窗口调成负数（内部原样保留），
// 但对外一律当作"一个字节也收不了"。
static void test_recv_window_clamps_negative()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 100);                      // 流级剩 65435
    fc.SetLocalInitialWindow(1);                 // delta = -65534 → 流级变 -99
    CHECK(fc.RecvWindow(1) == 0, "负窗口对外 clamp 到 0");
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
    test_conn_should_update_uses_connection_initial();
    test_recv_window_unknown_stream_respects_connection();
    test_recv_window_clamps_negative();
    test_pop_credit_returns_consumed_amount();
    test_should_update_below_threshold();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
