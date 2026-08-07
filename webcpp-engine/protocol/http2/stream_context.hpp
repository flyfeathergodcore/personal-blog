#pragma once
#include "protocol/context.hpp"
#include "protocol/session_region.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <memory>
#include <cstddef>
#include "coro/event_loop.h"

// ── H2WsWakeup ──
//
// H2 WebSocket（RFC 8441）的"定时唤醒"机制（coro 版，替代原 asio::steady_timer）。
// 等待侧（WS 协程）用 loop_->wait_timer_cancelable(ms, h, timer_id) 挂起，
// 推送侧（session 主循环的 OnData/OnRstStream）用 loop_->cancel_timer(timer_id)
// 即时唤醒：
//   - cancel_timer 返回 true  → 定时器作废、由推送侧负责 resume 等待中的 WS 协程
//   - cancel_timer 返回 false → 定时器已到期/已被消费，WS 协程的恢复已安排，勿重复唤醒
// 约定：timer_id 用等待协程的句柄地址编码（见 ws_connection_h2.hpp 的 WakeAwaiter），
// 推送侧可凭 timer_id 反解出句柄并 post 回事件循环。等待侧与推送侧运行在同一个
// worker 事件循环（单线程），故除现有 ws_data_queue_ 外无需额外加锁。
//
struct H2WsWakeup {
    coro::EventLoop* loop = nullptr;   // 等待侧所在事件循环；nullptr = 无等待者
    std::size_t timer_id = 0;          // 可取消定时器 id；0 = 未注册
};

// ── H2StreamContext ──
//
// HTTP/2 stream state.  Implements the Context interface so middleware
// and handler can process requests identically to HTTP/1.1.
//
class H2StreamContext : public Context {
public:
    // 默认构造函数。
    H2StreamContext() = default;

    // 设置请求方法（:method 伪头）。
    void SetMethod(std::string_view m);
    // 设置请求路径（:path 伪头）。
    void SetPath(std::string_view p);
    // 添加请求头：伪头特殊处理，普通头存入数组并跟踪 Content-Length。
    void AddHeader(std::string_view name, std::string_view value);
    // 追加请求体数据（跨多个 DATA 帧累积）。
    void AppendBody(const uint8_t* data, size_t len);

    // ── Context interface ──
    // H2 流无需按字节喂入解析器，直接返回完成。
    ParseResult Feed(const char*, size_t) override { return ParseResult::Complete; }
    // 获取请求方法。
    std::string_view Method()  const override;
    // 获取请求路径。
    std::string_view Path()    const override;
    // 获取协议版本。
    std::string_view Version() const override { return "HTTP/2"; }
    // 是否 HTTP/2 请求。
    bool IsHttp2() const override { return true; }
    // 按名称查找请求头。
    std::string_view Header(std::string_view key) const override;
    // 获取完整请求体。
    std::string_view Body()   const override;
    // 返回请求头数量。
    int HeaderCount() const override { return header_count_; }
    // 按下标取第 i 个请求头的 (名称, 值) 对，越界返回空对。
    std::pair<std::string_view, std::string_view> HeaderAt(int i) const override {
        if (i < 0 || i >= header_count_) return {};
        auto* r = Pool();
        return r ? std::pair{r->ToView(headers_[i].name), r->ToView(headers_[i].value)}
                 : std::pair<std::string_view, std::string_view>{};
    }

    /// 返回请求头中的 Content-Length 值。
    size_t ContentLength() const { return content_length_; }

    // ── Response body source (for DATA frames) ──
    const char* resp_body_ = nullptr;
    size_t resp_body_len_ = 0;
    size_t resp_body_off_ = 0;
    std::vector<char> file_buf_;   // for sendfile responses

    // ── SSE streaming state ──
    bool sse_active_ = false;
    int  push_interval_ms_ = 1000;
    std::string sse_payload_;

    // ── H2 WebSocket (RFC 8441 Extended CONNECT) ──
    bool ws_extended_ = false;    // true when :protocol: websocket detected
    bool ws_active_ = false;      // true when WS handler is running
    bool ws_closed_ = false;      // true when WS close initiated
    std::deque<std::string> ws_data_queue_;   // filled by OnDataChunk, drained by H2WsConnection
    H2WsWakeup ws_wakeup_;                    // 唤醒机制：WS 协程等待、session 推送侧唤醒

    // ── Body size tracking ──
    size_t content_length_ = 0;

    // ── State ──
    bool handled_ = false;       // true when HandleStream has been spawned
    bool stream_closed_ = false; // true when RST_STREAM / stream close received

private:
    static constexpr int kMaxHeaders = 64;

    RegionOff method_;
    RegionOff path_;

    // 请求体累积缓冲（跨多个 DATA 帧拼接）。DATA 帧的数据来自 session 的
    // 读缓冲（read_buf_），是瞬态的，必须拷贝进这里才能跨帧存活。
    std::string body_buf_;

    struct Entry { RegionOff name; RegionOff value; };
    Entry headers_[kMaxHeaders];
    int header_count_ = 0;
};
