#include "http/protocol/response.hpp"
#include "http/protocol/session_region.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>

// ── Region-backed response ──

// 构造响应对象：将状态行（如 "HTTP/1.1 200 OK"）写入区域池缓冲区（H1 wire 格式）
// 参数：code - HTTP 状态码；region - 会话区域池（响应数据写入点）
Response::Response(int code, SessionRegion& region)
    : region_(&region)
    , begin_off_(region.Used())
    , code_(code)
{
    region_->Write("HTTP/1.1 ");
    switch (code) {
        case 200: region_->Write("200 OK"); break;
        case 206: region_->Write("206 Partial Content"); break;
        case 301: region_->Write("301 Moved Permanently"); break;
        case 302: region_->Write("302 Found"); break;
        case 307: region_->Write("307 Temporary Redirect"); break;
        case 308: region_->Write("308 Permanent Redirect"); break;
        case 204: region_->Write("204 No Content"); break;
        case 304: region_->Write("304 Not Modified"); break;
        case 101: region_->Write("101 Switching Protocols"); break;
        case 400: region_->Write("400 Bad Request"); break;
        case 403: region_->Write("403 Forbidden"); break;
        case 404: region_->Write("404 Not Found"); break;
        case 426: region_->Write("426 Upgrade Required"); break;
        case 501: region_->Write("501 Not Implemented"); break;
        default:  region_->Write("500 Internal Server Error"); break;
    }
    region_->WriteCRLF();
}

// ── Raw (pre-built wire, no region) ──

// 构造原始响应：使用调用方预构建的完整 wire 数据，不写入区域池
// 参数：code - HTTP 状态码；wire - 预构建的原始响应字节流
Response Response::Raw(int code, std::string wire)
{
    Response r;
    r.code_ = code;
    r.raw_wire_ = std::move(wire);
    r.raw_mode_ = true;
    return r;
}

// ── Header building ──

// 写入一个字符串值响应头：H1 直写区域池 wire 格式，H2 存结构化存储
// 参数：key - 头部名称；value - 头部值
void Response::Header(std::string_view key, std::string_view value) {
    // Wire format — H1 reads this, H2 skips it entirely.
    if (!region_->StructuredMode()) {
        region_->Write(key);
        region_->Write(": ");
        region_->Write(value);
        region_->WriteCRLF();
    }

    // Structured headers — H2 reads this via HeaderAt().
    // H1 never enters this branch (StructuredMode is false).
    if (region_->StructuredMode()) {
        if (!hdr_)
            hdr_ = std::make_unique<HeaderStorage>();
        if (hdr_->header_count_ < kMaxHeaders) {
            auto& p = hdr_->pending_[hdr_->header_count_];
            p.name_len = static_cast<uint8_t>(std::min(key.size(), sizeof(p.name) - 1));
            std::memcpy(p.name, key.data(), p.name_len);
            p.value_len = static_cast<uint8_t>(std::min(value.size(),
                                                         sizeof(p.value) - 1));
            std::memcpy(p.value, value.data(), p.value_len);
            p.is_int = false;
            hdr_->header_count_++;
        }
    }
}

// 写入一个整数值响应头（H1 直写 wire，H2 存结构化存储）
// 参数：key - 头部名称；value - 整数头部值
void Response::Header(std::string_view key, uint64_t value) {
    if (!region_->StructuredMode()) {
        region_->Write(key);
        region_->Write(": ");
        region_->WriteUint(value);
        region_->WriteCRLF();
    }

    if (region_->StructuredMode()) {
        if (!hdr_)
            hdr_ = std::make_unique<HeaderStorage>();
        if (hdr_->header_count_ < kMaxHeaders) {
            auto& p = hdr_->pending_[hdr_->header_count_];
            p.name_len = static_cast<uint8_t>(std::min(key.size(), sizeof(p.name) - 1));
            std::memcpy(p.name, key.data(), p.name_len);
            p.int_len = static_cast<uint8_t>(std::snprintf(
                reinterpret_cast<char*>(p.int_buf), sizeof(p.int_buf),
                "%lu", (unsigned long)value));
            p.is_int = true;
            hdr_->header_count_++;
        }
    }
}

// ── Cached HTTP-date (shared with h2 session) ──

// 获取缓存的本秒 HTTP-date 字符串（RFC 7231 格式），每秒只重新格式化一次
// 参数：无；返回：形如 "Sun, 06 Nov 1994 08:49:37 GMT" 的日期字符串
std::string_view CachedDate()
{
    static time_t last = 0;
    static char buf[64];
    auto now = ::time(nullptr);
    if (now != last) {
        last = now;
        struct tm tm;
        ::gmtime_r(&now, &tm);
        ::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    }
    return {buf, strlen(buf)};
}

// 结束响应头：补充 Date/Connection（仅 H1 wire），写入空行并记录头部结束偏移
// 参数：无
void Response::EndHeaders() {
    if (region_) {
        // Date + Connection — H1 wire only.
        // H2 reads these from structured storage if needed (Date added
        // explicitly in HandleStream; Connection is hop-by-hop for H2).
        if (!region_->StructuredMode()) {
            auto date = CachedDate();
            region_->Write("Date: ");
            region_->Write(date);
            region_->WriteCRLF();
            if (!sse_ && !ws_upgrade_)
                region_->Write("Connection: keep-alive\r\n");
        }
    }
    region_->WriteCRLF();
    header_end_ = region_->Used();

    // Structured headers: no DupOff to Region.  HeaderAt() reads directly
    // from hdr_->pending_[] inline buffers, which live until Response is
    // destroyed (after HandleStream finishes).
}

// ── Body ──

// 设置外部内存中的响应体（不复制到区域池）
// 参数：data - 响应体数据指针；len - 响应体长度（字节）
void Response::Body(const char* data, size_t len) {
    ext_body_ = data;
    ext_body_len_ = len;
}

// 设置文件响应体：以文件描述符方式发送，支持 Range 部分内容（206）
// 参数：fd - 文件描述符；file_size - 文件总大小；range_offset - 起始偏移；range_len - 传输长度
void Response::BodyFile(int fd, size_t file_size,
                        size_t range_offset, size_t range_len) {
    fd_ = fd;
    file_size_ = file_size;
    file_range_offset_ = range_offset;
    file_range_len_ = range_len;
}

// ── Queries ──

// 判断响应是否为空（既未绑定区域池也非原始模式）
// 参数：无
bool Response::IsNone() const {
    return !region_ && !raw_mode_;
}

// 判断响应是否为文件响应（已绑定区域池且设置了文件描述符）
// 参数：无
bool Response::IsFile() const {
    return region_ && fd_ >= 0;
}

// 获取响应头在 wire 上的字节视图（raw 模式返回原始数据，否则取自区域池）
// 参数：无
std::string_view Response::HeaderWire() const {
    if (raw_mode_) return raw_wire_;
    if (region_)
        return {region_->Data() + begin_off_, header_end_ - begin_off_};
    return {};
}

// 获取响应体字节视图（优先外部 body，其次区域池中头部之后的数据）
// 参数：无
std::string_view Response::BodyWire() const {
    if (ext_body_)     // external body (FileCache, not in region)
        return {ext_body_, ext_body_len_};
    if (region_ && header_end_ > 0 && region_->Used() > header_end_)
        return {region_->Data() + header_end_, region_->Used() - header_end_};
    return {};
}

// ── Structured header access ──
//
// Reads directly from the heap-allocated pending_ buffers (H2 only).
// No DupOff/RegionOff needed — HeaderStorage lives until Response is
// destroyed, which outlives HandleStream's consumption.

// 获取结构化响应头数量（H2 使用）
// 参数：无
int Response::HeaderCount() const {
    return hdr_ ? hdr_->header_count_ : 0;
}

// 获取第 i 个结构化响应头（名称, 值）对（H2 使用）
// 参数：i - 头部索引；返回：越界返回空对
std::pair<std::string_view, std::string_view> Response::HeaderAt(int i) const {
    if (!hdr_ || i < 0 || i >= hdr_->header_count_)
        return {};
    auto& p = hdr_->pending_[i];
    if (p.is_int)
        return {{p.name, p.name_len},
                {reinterpret_cast<const char*>(p.int_buf), p.int_len}};
    return {{p.name, p.name_len}, {p.value, p.value_len}};
}

// ── SSE stream factory ──

// 构造 Server-Sent Events 流式响应（200 + text/event-stream）
// 参数：region - 会话区域池；min_interval_ms - 最小推送间隔（毫秒，下限 200）
Response Response::SSEStream(SessionRegion& region, int min_interval_ms)
{
    Response resp(200, region);
    resp.sse_ = true;  // before EndHeaders so Date/Connection handled correctly
    resp.Header("Content-Type", "text/event-stream");
    resp.Header("Cache-Control", "no-cache");
    resp.EndHeaders();
    resp.push_interval_ms_ = std::max(min_interval_ms, 200);  // floor 200ms
    return resp;
}

// ── WebSocket upgrade factory ──

// 构造 WebSocket 升级响应（101 Switching Protocols），携带 Sec-WebSocket-Accept
// 参数：region - 会话区域池；accept - 计算得到的 Sec-WebSocket-Accept 值
Response Response::WebSocketUpgrade(SessionRegion& region, std::string accept)
{
    Response resp(101, region);
    resp.ws_upgrade_ = true;
    resp.ws_accept_ = accept;
    resp.Header("Upgrade", "websocket");
    resp.Header("Connection", "Upgrade");
    resp.Header("Sec-WebSocket-Accept", std::move(accept));
    resp.EndHeaders();
    return resp;
}

// ── Error factory ──

// 构造错误响应：生成 HTML 错误页并写入区域池
// 参数：code - HTTP 错误状态码；region - 会话区域池
Response Response::Error(int code, SessionRegion& region)
{
    const char* text;
    switch (code) {
        case 400: text = "Bad Request"; break;
        case 403: text = "Forbidden"; break;
        case 404: text = "Not Found"; break;
        case 416: text = "Range Not Satisfiable"; break;
        case 501: text = "Not Implemented"; break;
        default:  text = "Error"; break;
    }

    char body[128];
    int body_len = std::snprintf(body, sizeof(body),
                                 "<h1>%d %s</h1>", code, text);
    if (body_len < 0) body_len = 0;

    Response resp(code, region);
    resp.Header("Content-Type", "text/html");
    resp.Header("Content-Length", static_cast<uint64_t>(body_len));
    resp.EndHeaders();
    region.Write({body, static_cast<size_t>(body_len)});
    return resp;
}
