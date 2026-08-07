#pragma once
#include "protocol/session_region.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class SessionRegion;

class Response {
public:
    static constexpr int kMaxHeaders = 32;

    // 默认构造（公开）：coro::Task<Response> 的 promise_type 需要默认构造结果，
    // 故由 private 放开。业务侧仍应使用 None/Raw/Error 等工厂。
    Response() = default;
    // 构造一个空响应（无区域池、非原始模式），业务侧默认返回
    // 参数：无
    static Response None() { return {}; }
    // 构造响应对象：向区域池写入状态行（H1 wire 格式）
    // 参数：code - HTTP 状态码；region - 会话区域池
    Response(int code, SessionRegion& region);
    // 构造原始响应：使用预构建的完整 wire 数据，不写区域池
    // 参数：code - HTTP 状态码；wire - 预构建的原始响应字节流
    static Response Raw(int code, std::string wire);
    // 构造错误响应：生成 HTML 错误页写入区域池
    // 参数：code - HTTP 错误状态码；region - 会话区域池
    static Response Error(int code, SessionRegion& region);
    // 构造 SSE 流式响应（200 + text/event-stream）
    // 参数：region - 会话区域池；min_interval_ms - 最小推送间隔（毫秒，下限 200）
    static Response SSEStream(SessionRegion& region, int min_interval_ms);

    // ── WebSocket upgrade response (101) ──
    // 构造 WebSocket 升级响应（101），携带 Sec-WebSocket-Accept
    // 参数：region - 会话区域池；accept - 计算得到的 Sec-WebSocket-Accept 值
    static Response WebSocketUpgrade(SessionRegion& region,
                                      std::string accept);
    // 判断是否为 WebSocket 升级响应（101）
    // 参数：无
    bool IsWebSocket() const { return ws_upgrade_; }
    // 获取 Sec-WebSocket-Accept 值
    // 参数：无
    std::string_view WsAccept() const { return ws_accept_; }

    // ── Header building ──
    // 写入一个字符串值响应头（H1 直写 wire，H2 存结构化存储）
    // 参数：key - 头部名称；value - 头部值
    void Header(std::string_view key, std::string_view value);
    // 写入一个整数值响应头（H1 直写 wire，H2 存结构化存储）
    // 参数：key - 头部名称；value - 整数头部值
    void Header(std::string_view key, uint64_t value);
    // 结束响应头：补充 Date/Connection 并写入空行
    // 参数：无
    void EndHeaders();

    // ── Structured header access (for H2, consumes what Header() stored) ──
    // 获取结构化响应头数量（H2 使用）
    // 参数：无
    int HeaderCount() const;
    // 获取第 i 个结构化响应头（名称, 值）对（H2 使用）
    // 参数：i - 头部索引；返回：越界返回空对
    std::pair<std::string_view, std::string_view> HeaderAt(int i) const;

    // ── Body ──
    // 设置外部内存中的响应体（不复制）
    // 参数：data - 响应体数据指针；len - 长度（字节）
    void Body(const char* data, size_t len);
    // 设置外部内存中的响应体（string_view 便捷重载）
    // 参数：s - 响应体数据
    void Body(std::string_view s) { Body(s.data(), s.size()); }

    // 以文件描述符方式设置文件响应体，支持 Range 部分内容（206）
    // 参数：fd - 文件描述符；file_size - 文件总大小；range_offset - 起始偏移（默认 0）；range_len - 传输长度（默认 0 全量）
    void BodyFile(int fd, size_t file_size,
                  size_t range_offset = 0, size_t range_len = 0);

    // ── Queries ──
    // 判断响应是否为空（未绑定区域池且非原始模式）
    // 参数：无
    bool IsNone() const;
    // 判断响应是否为文件响应
    // 参数：无
    bool IsFile() const;
    // 判断是否为 SSE 流式响应
    // 参数：无
    bool IsStream() const { return sse_; }
    // 获取 SSE 推送间隔（毫秒）
    // 参数：无
    int PushIntervalMs() const { return push_interval_ms_; }
    // 获取 HTTP 状态码
    // 参数：无
    int StatusCode() const { return code_; }
    // 获取响应头 wire 字节视图
    // 参数：无
    std::string_view HeaderWire() const;
    // 获取响应体字节视图
    // 参数：无
    std::string_view BodyWire() const;
    // 获取文件响应描述符（非文件响应为 -1）
    // 参数：无
    int Fd() const { return fd_; }
    // 获取文件总大小
    // 参数：无
    size_t FileSize() const { return file_size_; }
    // 获取文件 Range 起始偏移
    // 参数：无
    size_t FileRangeOffset() const { return file_range_offset_; }
    // 获取文件 Range 传输长度
    // 参数：无
    size_t FileRangeLen() const { return file_range_len_; }

private:
    SessionRegion* region_ = nullptr;
    size_t begin_off_  = 0;
    size_t header_end_ = 0;
    int code_ = 200;

    static constexpr int kMaxFieldLen = 128;
    struct PendingHeader {
        char    name[kMaxFieldLen];
        uint8_t name_len = 0;
        char    value[kMaxFieldLen];
        uint8_t value_len = 0;
        uint8_t int_buf[24];       // formatted uint64_t
        uint8_t int_len   = 0;
        bool    is_int    = false;
    };

    // Heap-allocated header storage — only allocated when StructuredMode (H2).
    // H1 never hits this path, keeping sizeof(Response) = ~112 bytes.
    // HeaderAt() reads directly from these inline buffers (no RegionOff/DupOff).
    struct HeaderStorage {
        PendingHeader pending_[kMaxHeaders];
        int           header_count_ = 0;
    };
    std::unique_ptr<HeaderStorage> hdr_;

    const char* ext_body_ = nullptr;
    size_t ext_body_len_  = 0;
    int fd_ = -1;
    size_t file_size_ = 0;
    size_t file_range_offset_ = 0;
    size_t file_range_len_ = 0;

    std::string raw_wire_;
    bool raw_mode_ = false;

    bool sse_ = false;
    int push_interval_ms_ = 1000;

    bool ws_upgrade_ = false;
    std::string ws_accept_;
};

// 获取缓存的本秒 HTTP-date 字符串（RFC 7231 格式），每秒只重新格式化一次
// 参数：无；返回：形如 "Sun, 06 Nov 1994 08:49:37 GMT" 的日期字符串
std::string_view CachedDate();
