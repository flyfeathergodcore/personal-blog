#include "http/protocol/http2/stream_context.hpp"
#include <cstring>

// 设置请求方法（:method 伪头）。
// 参数：m - 方法名，如 GET/POST
void H2StreamContext::SetMethod(std::string_view m)
{
    auto* pool = Pool();
    if (pool) method_ = pool->DupOff(m);
}

// 设置请求路径（:path 伪头）。
// 参数：p - 请求路径，如 /
void H2StreamContext::SetPath(std::string_view p)
{
    auto* pool = Pool();
    if (pool) path_ = pool->DupOff(p);
}

// 添加一个请求头：伪头（:method/:path/:protocol 等）特殊处理，普通头存入数组，
// 并跟踪 Content-Length 用于限制请求体大小。
// 参数：name - 头名称；value - 头值
void H2StreamContext::AddHeader(std::string_view name, std::string_view value)
{
    // ── Pseudo-headers: handle without storing ──
    if (name == ":method")  { SetMethod(value); return; }
    if (name == ":path")    { SetPath(value);   return; }
    if (name == ":protocol" && value == "websocket") { ws_extended_ = true; return; }
    if (name == ":authority" || name == ":scheme") return;

    if (header_count_ >= kMaxHeaders) return;
    auto* pool = Pool();
    if (!pool) return;

    headers_[header_count_].name  = pool->DupOff(name);
    headers_[header_count_].value = pool->DupOff(value);
    header_count_++;

    // Track Content-Length for body size limiting
    if (name == "content-length") {
        content_length_ = 0;
        for (char c : value) {
            if (c < '0' || c > '9') break;
            content_length_ = content_length_ * 10 + static_cast<size_t>(c - '0');
        }
    }
}

// 追加请求体数据到累积缓冲（跨多个 DATA 帧拼接）。
// 参数：data - 待追加的数据指针；len - 长度（字节）
void H2StreamContext::AppendBody(const uint8_t* data, size_t len)
{
    if (len == 0) return;
    // 追加语义：一个请求体可能横跨多个 DATA 帧（如 curl 按 ~16KB 分帧，
    // 或 H2 窗口/帧大小限制导致的分片）。逐帧追加，保证 Body() 返回完整请求体，
    // 而不是像原来那样每帧覆盖 body_（只保留最后一帧）。
    body_buf_.append(reinterpret_cast<const char*>(data), len);
}

// 获取请求方法。
std::string_view H2StreamContext::Method() const
{
    auto* r = Pool();
    return r ? r->ToView(method_) : std::string_view{};
}

// 获取请求路径。
std::string_view H2StreamContext::Path() const
{
    auto* r = Pool();
    return r ? r->ToView(path_) : std::string_view{};
}

// 获取完整的请求体（跨帧累积后的结果）。
std::string_view H2StreamContext::Body() const
{
    return std::string_view(body_buf_);
}

// 按名称查找请求头，未找到返回空视图。
// 参数：key - 头名称
std::string_view H2StreamContext::Header(std::string_view key) const
{
    auto* r = Pool();
    if (!r) return {};
    for (int i = 0; i < header_count_; i++)
    {
        if (r->ToView(headers_[i].name) == key)
            return r->ToView(headers_[i].value);
    }
    return {};
}
