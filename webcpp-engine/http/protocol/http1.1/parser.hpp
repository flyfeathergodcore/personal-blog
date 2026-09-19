#pragma once
#include "http/protocol/context.hpp"
#include "http/protocol/session_region.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class H1Parser : public Context {
public:
    // 默认构造解析器
    H1Parser();
    // 默认析构解析器
    ~H1Parser() override;

    // 向解析器喂入数据，按状态机增量解析 HTTP/1.1 请求
    // 参数：data - 输入数据指针；len - 输入长度（字节）；返回：解析结果
    ParseResult Feed(const char* data, size_t len) override;

    /// 复位整个 parser（per-message 字段 + 连接级 h2 检测标志）。
    /// 供会话池复用 H11Session 壳时调用：上一连接若在请求中途断开
    /// （state_ 停在 HEADERS/BODY），Feed 只对 DONE/ERROR 态自复位，
    /// 其余状态必须显式复位，否则新连接的首个请求会被旧状态机误解析。
    void Reset();

    // 设置对端 IP（连接建立时由会话层 getpeername 注入，连接级不变）
    // 参数：ip - IPv4 点分十进制字符串
    void SetPeerIp(std::string_view ip) { peer_ip_ = ip; }
    // 获取对端 IP（未注入返回空）
    // 参数：无
    std::string_view PeerIp() const override { return peer_ip_; }

    // 获取请求方法（如 GET/POST）
    // 参数：无
    std::string_view Method()  const override { return method_; }
    // 获取请求路径（区域池视图）
    // 参数：无
    std::string_view Path()    const override;
    // 获取 HTTP 版本
    // 参数：无
    std::string_view Version() const override { return version_; }
    // 按名称查询请求头
    // 参数：key - 头部名称
    std::string_view Header(const std::string_view key) const override;
    // 获取请求体（区域池视图）
    // 参数：无
    std::string_view Body()   const override;
    // 获取请求头总数
    // 参数：无
    int HeaderCount() const override { return header_count_; }
    // 获取第 i 个请求头（名称, 值）对
    // 参数：i - 头部索引
    std::pair<std::string_view, std::string_view> HeaderAt(int i) const override {
        if (i < 0 || i >= header_count_) return {};
        auto* r = Pool();
        return r ? std::pair{r->ToView(headers_[i].name), r->ToView(headers_[i].value)}
                 : std::pair<std::string_view, std::string_view>{};
    }

    /// True if the connection starts with HTTP/2 preface ("PRI * HTTP/2.0").
    bool IsH2() const { return h2_detected_; }

    /// Content-Length of the request body (0 if not set).
    size_t ContentLength() const { return content_length_; }

    /// 最近一次 Feed 从输入中消费的字节数（调用方据此保留未消费的 pipeline 残留）。
    size_t Consumed() const { return consumed_; }

    /// True 表示 parser 正处于两条请求之间（无跨请求的部分解析状态残留），
    /// 此时可以安全地 Reset 区域池；HEADERS/BODY 中途不能 Reset。
    bool IsIdle() const {
        return state_ != HEADERS && state_ != BODY;
    }

private:
    enum State : uint8_t {
        REQUEST_LINE,
        HEADERS,
        BODY,
        DONE,
        ERROR_STATE,
    };

    State state_ = REQUEST_LINE;

    static constexpr size_t kMaxLine = 4096;
    char line_buf_[kMaxLine];
    size_t line_len_ = 0;

    static constexpr int kMaxHeaders = 64;
    struct HeaderEntry { RegionOff name; RegionOff value; };
    HeaderEntry headers_[kMaxHeaders];
    int header_count_ = 0;

    std::string_view method_;
    std::string_view version_;
    RegionOff path_;
    RegionOff body_;
    size_t content_length_ = 0;
    size_t body_written_ = 0;

    bool h2_detected_ = false;
    bool message_complete_ = false;

    // 对端 IP（IPv4 点分十进制），连接建立时注入，连接级不变
    std::string peer_ip_;

    size_t consumed_ = 0;   // 最近一次 Feed 消费的字节数

    // 处理一行解析结果（按状态分发）
    // 参数：无；返回：true 成功，false 解析错误
    bool ProcessLine();
    // 解析请求行（方法 路径 版本）
    // 参数：无；返回：true 成功，false 格式错误
    bool ParseRequestLine();
    // 解析单个请求头行
    // 参数：无；返回：true 成功，false 格式错误
    bool ParseHeaderLine();
    // 拷贝请求体数据到区域池 body 区域
    // 参数：data - 输入数据；len - 可写字节数
    void WriteBody(const char* data, size_t len);

    /// 复位 per-message 解析状态（Feed 的 DONE/ERROR 自复位与公共 Reset()
    /// 共用）。不含 h2_detected_——连接级标志，仅公共 Reset()（复用前）清零。
    void ResetMessage();
};
