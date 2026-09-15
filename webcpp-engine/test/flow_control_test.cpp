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

// ── 发送账：连接窗口是所有流共用的 ──
static void test_send_window_is_min_of_stream_and_connection()
{
    H2FlowControl fc;
    fc.ConsumeSend(1, 60000);        // 流 1 扣 60000，连接也扣 60000

    CHECK(fc.SendWindow(1) == 5535, "流 1 剩 5535");
    CHECK(fc.SendWindow(0) == 5535, "连接级剩 5535");
    CHECK(fc.SendWindow(3) == 5535, "未用过的流 3 受连接窗口限制，也是 5535");
}

// ── 发送账：补连接窗口会同时放开所有流 ──
static void test_add_send_credit_connection_releases_all_streams()
{
    H2FlowControl fc;
    fc.ConsumeSend(1, 65535);        // 连接窗口耗尽
    CHECK(fc.SendWindow(1) == 0, "连接窗口耗尽 → 流 1 一个字节也发不出");
    CHECK(fc.SendWindow(3) == 0, "其他流同样发不出");

    fc.AddSendCredit(0, 1000);       // 只补连接
    CHECK(fc.SendWindow(3) == 1000, "连接窗口放开后其他流可用");
    CHECK(fc.SendWindow(1) == 0, "流 1 的【流级】窗口已耗尽，仍不可用");
}

// ── 发送账：补流窗口不影响连接，且流窗口不是瓶颈时不起作用 ──
static void test_add_send_credit_stream_does_not_touch_connection()
{
    H2FlowControl fc;
    fc.ConsumeSend(1, 10000);        // 流 1 与连接各扣 10000

    fc.AddSendCredit(1, 5000);       // 只补流 1
    CHECK(fc.SendWindow(1) == 55535,
          "流 1 的流级账已回到 60535，但连接账仍是 55535 → 取较小值");
    CHECK(fc.SendWindow(3) == 55535, "流 3 未补充，同样受连接窗口限制");

    fc.AddSendCredit(0, 5000);       // 再补连接
    CHECK(fc.SendWindow(1) == 60535, "两本账都放开后，流 1 才拿到 60535");
    CHECK(fc.SendWindow(3) == 60535, "流 3 也拿到 60535");
}

// ── 发送账：SETTINGS_INITIAL_WINDOW_SIZE 只动流级，且允许压成负数（§6.9.2）──
static void test_peer_settings_make_stream_window_negative()
{
    H2FlowControl fc;
    fc.ConsumeSend(1, 1000);         // 流 1 与连接各扣 1000

    fc.SetPeerInitialWindow(0);      // 对端把初始窗口降到 0
    CHECK(fc.SendWindow(1) == 0, "流 1 窗口被压成 0，不可发");
    CHECK(fc.SendWindow(0) == 64535, "连接级发送窗口不受 SETTINGS 影响");

    fc.AddSendCredit(1, 500);        // 对端补 500
    CHECK(fc.SendWindow(1) == 0,
          "原窗口是 -1000，补 500 后仍为负 → 依旧不可发");
    fc.AddSendCredit(1, 1000);       // 再补 1000
    CHECK(fc.SendWindow(1) == 500, "补够后恢复出 500 额度");
}

// ── 发送账：SETTINGS 不触碰接收账 ──
// 必须用【调小】。调大时流级被抬高到 130972，但 RecvWindow 取 min(流, 连接)，
// 连接账 65435 仍是瓶颈 —— 串不串账都返回 65435，断言恒真，什么也锁不住。
// 实测：造一个「对端 SETTINGS 误伤流级接收账」的实现，131072 版本 39 条全绿。
// 调小到 1000 则 delta = -64535，流级被压到 900，成为 min 的较小者，
// 串账立刻暴露成 900 ≠ 65435。
static void test_peer_settings_do_not_touch_recv_accounts()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 100);

    fc.SetPeerInitialWindow(1000);   // delta = -64535 → 误串账时 recv_[1] = 900
    CHECK(fc.RecvWindow(1) == 65435, "接收账完全不受对端 SETTINGS 影响");
    CHECK(fc.RecvWindow(0) == 65435, "连接级接收账同样不受影响");
}

// ── 生命周期：RemoveStream 清掉两本账 ──
static void test_remove_stream_clears_both_accounts()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 100);
    fc.ConsumeSend(1, 100);
    CHECK(fc.HasStream(1), "流 1 有账目");

    fc.RemoveStream(1);
    CHECK(!fc.HasStream(1), "移除后两本账都没了");
    // 流级账没了，RecvWindow 走未知流分支：流级按初始窗口满额 65535，
    // 但连接账已被这次 ConsumeRecv 扣掉 100，故取 min 后是 65435 而非 65535。
    // （这一条同时也钉住了未知流分支的 min——写成 65535 会失败。）
    CHECK(fc.RecvWindow(1) == 65435, "接收账回落到初始满额，但仍受连接窗口约束");
    CHECK(fc.SendWindow(0) == 65435, "连接级发送账不受影响（只扣了一次 100）");
}

// ── 生命周期：RemoveStream(0) 是空操作，不能误删连接账 ──
static void test_remove_stream_zero_is_noop()
{
    H2FlowControl fc;
    fc.ConsumeRecv(1, 1000);
    fc.RemoveStream(0);
    CHECK(fc.RecvWindow(0) == 64535, "连接级接收账未被清掉");
}

static void test_send_credit_rejects_window_overflow()
{
    H2FlowControl fc;
    CHECK(!fc.AddSendCredit(0, 0x7fffffffU),
          "连接级发送窗口超过 2^31-1 时拒绝更新");
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
    test_send_window_is_min_of_stream_and_connection();
    test_add_send_credit_connection_releases_all_streams();
    test_add_send_credit_stream_does_not_touch_connection();
    test_peer_settings_make_stream_window_negative();
    test_peer_settings_do_not_touch_recv_accounts();
    test_remove_stream_clears_both_accounts();
    test_remove_stream_zero_is_noop();
    test_send_credit_rejects_window_overflow();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
