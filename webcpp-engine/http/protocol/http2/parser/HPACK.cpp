#include "http/protocol/http2/parser/HPACK.hpp"
#include "http/protocol/http2/stream_context.hpp"
#include "http/protocol/session_region.hpp"
#include <cstring>
#include <algorithm>
#include <array>
#include <cassert>

// ═══════════════════════════════════════════════════════════════
// Static table — RFC 7541 Appendix A (61 entries, 1-indexed)
// ═══════════════════════════════════════════════════════════════

// clang-format off
static constexpr const char* kStaticNames[61] = {
    ":authority",                // 1
    ":method",                   // 2
    ":method",                   // 3
    ":path",                     // 4
    ":path",                     // 5
    ":scheme",                   // 6
    ":scheme",                   // 7
    ":status",                   // 8
    ":status",                   // 9
    ":status",                   // 10
    ":status",                   // 11
    ":status",                   // 12
    ":status",                   // 13
    ":status",                   // 14
    "accept-charset",            // 15
    "accept-encoding",           // 16
    "accept-language",           // 17
    "accept-ranges",             // 18
    "accept",                    // 19
    "access-control-allow-origin", // 20
    "age",                       // 21
    "allow",                     // 22
    "authorization",             // 23
    "cache-control",             // 24
    "content-disposition",       // 25
    "content-encoding",          // 26
    "content-language",          // 27
    "content-length",            // 28
    "content-location",          // 29
    "content-range",             // 30
    "content-type",              // 31
    "cookie",                    // 32
    "date",                      // 33
    "etag",                      // 34
    "expect",                    // 35
    "expires",                   // 36
    "from",                      // 37
    "host",                      // 38
    "if-match",                  // 39
    "if-modified-since",         // 40
    "if-none-match",             // 41
    "if-range",                  // 42
    "if-unmodified-since",       // 43
    "last-modified",             // 44
    "link",                      // 45
    "location",                  // 46
    "max-forwards",              // 47
    "proxy-authenticate",        // 48
    "proxy-authorization",       // 49
    "range",                     // 50
    "referer",                   // 51
    "refresh",                   // 52
    "retry-after",               // 53
    "server",                    // 54
    "set-cookie",                // 55
    "strict-transport-security", // 56
    "transfer-encoding",         // 57
    "user-agent",                // 58
    "vary",                      // 59
    "via",                       // 60
    "www-authenticate",          // 61
};

static constexpr const char* kStaticValues[61] = {
    "",          // 1:  :authority
    "GET",       // 2:  :method GET
    "POST",      // 3:  :method POST
    "/",         // 4:  :path /
    "/index.html", // 5:  :path /index.html
    "http",      // 6:  :scheme http
    "https",     // 7:  :scheme https
    "200",       // 8:  :status 200
    "204",       // 9:  :status 204
    "206",       // 10: :status 206
    "304",       // 11: :status 304
    "400",       // 12: :status 400
    "404",       // 13: :status 404
    "500",       // 14: :status 500
    "",          // 15: accept-charset
    "",          // 16: accept-encoding
    "",          // 17: accept-language
    "",          // 18: accept-ranges
    "",          // 19: accept
    "",          // 20: access-control-allow-origin
    "",          // 21: age
    "",          // 22: allow
    "",          // 23: authorization
    "",          // 24: cache-control
    "",          // 25: content-disposition
    "",          // 26: content-encoding
    "",          // 27: content-language
    "",          // 28: content-length
    "",          // 29: content-location
    "",          // 30: content-range
    "",          // 31: content-type
    "",          // 32: cookie
    "",          // 33: date
    "",          // 34: etag
    "",          // 35: expect
    "",          // 36: expires
    "",          // 37: from
    "",          // 38: host
    "",          // 39: if-match
    "",          // 40: if-modified-since
    "",          // 41: if-none-match
    "",          // 42: if-range
    "",          // 43: if-unmodified-since
    "",          // 44: last-modified
    "",          // 45: link
    "",          // 46: location
    "",          // 47: max-forwards
    "",          // 48: proxy-authenticate
    "",          // 49: proxy-authorization
    "",          // 50: range
    "",          // 51: referer
    "",          // 52: refresh
    "",          // 53: retry-after
    "",          // 54: server
    "",          // 55: set-cookie
    "",          // 56: strict-transport-security
    "",          // 57: transfer-encoding
    "",          // 58: user-agent
    "",          // 59: vary
    "",          // 60: via
    "",          // 61: www-authenticate
};
// clang-format on

// ═══════════════════════════════════════════════════════════════
// Huffman — RFC 7541 Appendix B (code lengths from hpack library)
//
// Canonical Huffman code generation at init time.
// ═══════════════════════════════════════════════════════════════

namespace {

static constexpr uint8_t kHuffLens[256] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
     6, 10, 10, 12, 13,  6,  8, 11, 10, 10,  8, 11,  8,  6,  6,  6,
     5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  7,  8, 15,  6, 12, 10,
    13,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
     7,  7,  7,  7,  7,  7,  7,  7,  8,  7,  8, 13, 19, 13, 14,  6,
    15,  5,  6,  5,  6,  5,  6,  6,  6,  5,  7,  7,  6,  6,  6,  5,
     6,  7,  6,  5,  5,  6,  7,  7,  7,  7,  7, 15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
};

struct HuffmanEntry { uint32_t code; uint8_t len; };
static HuffmanEntry kHuffTable[256];

// 程序启动时按 RFC 7541 附录 B 的码长表生成规范哈夫曼码表。
// 采用 canonical 编码：按码长递增依次分配前缀码，结果存于 kHuffTable 供解码使用。
__attribute__((constructor)) static void InitHuffmanTable()
{
    uint16_t bl_count[33] = {0};
    for (int i = 0; i < 256; i++)
        bl_count[kHuffLens[i]]++;

    uint32_t code = 0;
    uint32_t next_code[33] = {0};
    for (int bits = 1; bits <= 32; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    for (int i = 0; i < 256; i++) {
        uint8_t len = kHuffLens[i];
        if (len > 0) {
            kHuffTable[i].code = next_code[len]++;
            kHuffTable[i].len = len;
        }
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// Static table lookup
// ═══════════════════════════════════════════════════════════════

// 按 1 起始下标查静态表（RFC 7541 附录 A）中的名称，越界返回空视图。
// 参数：index - 1~61 的静态表下标
std::string_view HpackDecoder::StaticName(int index)
{
    if (index < 1 || index > 61) return {};
    return kStaticNames[index - 1];
}

// 按 1 起始下标查静态表中的值，越界返回空视图。
// 参数：index - 1~61 的静态表下标
std::string_view HpackDecoder::StaticValue(int index)
{
    if (index < 1 || index > 61) return {};
    return kStaticValues[index - 1];
}

// ═══════════════════════════════════════════════════════════════
// Dynamic table management
// ═══════════════════════════════════════════════════════════════

// 计算一条动态表条目占用的表空间：32 + 名称长度 + 值长度（RFC 7541 §4.1）。
// 参数：n - 头名称；v - 头值
size_t HpackDecoder::EntryOverhead(std::string_view n, std::string_view v)
{
    return 32 + n.size() + v.size();
}

// 从表尾逐条淘汰动态表条目，直到当前表大小不超过目标值（RFC 7541 §4.4）。
// 参数：target_size - 目标表大小上限
void HpackDecoder::EvictTo(uint32_t target_size)
{
    while (current_table_size_ > target_size && !dynamic_table_.empty()) {
        auto& last = dynamic_table_.back();
        current_table_size_ -= EntryOverhead(last.name, last.value);
        dynamic_table_.pop_back();
    }
}

// 设置动态表最大尺寸（来自对端 SETTINGS HEADER_TABLE_SIZE），并按需淘汰旧条目。
// 参数：size - 新的最大表尺寸
void HpackDecoder::SetMaxTableSize(uint32_t size)
{
    max_table_size_ = size;
    EvictTo(max_table_size_);
}

// ═══════════════════════════════════════════════════════════════
// Integer decoding — RFC 7541 §5.1
// ═══════════════════════════════════════════════════════════════

// 解码 HPACK 整数（RFC 7541 §5.1）：先取首字节前缀位，满掩码时继续读 7 位续字节。
// 参数：data - 输入指针（引用，解码后前移）；len - 剩余长度（引用，解码后减少）；
//       prefix_bits - 前缀位数（决定首字节掩码）
uint32_t HpackDecoder::DecodeInteger(const uint8_t*& data, size_t& len, uint8_t prefix_bits)
{
    if (len == 0) return 0;

    uint8_t mask = static_cast<uint8_t>((1u << prefix_bits) - 1);
    uint32_t value = data[0] & mask;
    size_t consumed = 1;

    if (value == static_cast<uint32_t>(mask)) {
        uint32_t m = 0;
        while (consumed < len) {
            uint8_t byte = data[consumed++];
            value += static_cast<uint32_t>(byte & 0x7f) << m;
            m += 7;
            if (!(byte & 0x80)) break;
        }
    }

    data += consumed;
    len -= consumed;
    return value;
}

// ═══════════════════════════════════════════════════════════════
// String decoding — RFC 7541 §5.2
// ═══════════════════════════════════════════════════════════════

// 解码 HPACK 字符串（RFC 7541 §5.2）：先读 7 位前缀长度，再按是否哈夫曼编码解码，
// 结果拷贝到会话区域（SessionRegion）以跨帧存活。
// 参数：data - 输入指针（引用，解码后前移）；len - 剩余长度（引用，解码后减少）；
//       region - 用于存放解码字符串的会话区域
std::string_view HpackDecoder::DecodeString(const uint8_t*& data, size_t& len,
                                              SessionRegion& region)
{
    if (len == 0) return {};

    bool huffman = (data[0] & 0x80) != 0;
    uint32_t str_len = DecodeInteger(data, len, 7);

    if (str_len > len) {
        len = 0;
        return {};
    }

    std::string_view result;
    if (huffman) {
        auto decoded = HuffmanDecode(data, str_len);
        if (decoded.empty()) {
            len = 0;
            return {};
        }
        result = region.Dup(decoded);
    } else {
        result = region.Dup({reinterpret_cast<const char*>(data), str_len});
    }

    data += str_len;
    len -= str_len;
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Huffman decoding
// ═══════════════════════════════════════════════════════════════

// 哈夫曼解码（RFC 7541 §5.2）：用预生成的码表逐符号匹配，并处理末尾 <=7 位的全 1 填充。
// 参数：data - 编码数据指针；len - 编码数据长度；返回解码后的字符串（失败返回空串）
std::string HpackDecoder::HuffmanDecode(const uint8_t* data, size_t len)
{
    if (len == 0) return {};

    std::string result;
    uint64_t buf = 0;
    int bits = 0;
    const uint8_t* end = data + len;

    while (true) {
        // Fill bit window from input
        while (bits <= 32 && data < end) {
            buf = (buf << 8) | *data++;
            bits += 8;
        }

        // Try to match against all symbols
        bool matched = false;
        for (int sym = 0; sym < 256; sym++) {
            auto& entry = kHuffTable[sym];
            if (entry.len == 0 || entry.len > static_cast<uint8_t>(bits))
                continue;

            uint32_t peek = static_cast<uint32_t>(
                (buf >> (bits - entry.len)) & ((1ull << entry.len) - 1));

            if (peek == entry.code) {
                result.push_back(static_cast<char>(sym));
                bits -= entry.len;
                matched = true;
                break;
            }
        }

        if (matched) continue;

        // No match — check for EOS padding (all-1s, <= 7 bits)
        if (data >= end && bits <= 7) {
            if (bits > 0) {
                uint32_t pad = static_cast<uint32_t>(
                    buf & ((1ull << bits) - 1));
                uint32_t all_ones = (1u << bits) - 1;
                if (pad != all_ones)
                    return {};
            }
            return result;
        }


        return {};
    }
}

// ═══════════════════════════════════════════════════════════════
// Combined index lookup
// ═══════════════════════════════════════════════════════════════

// 按合并索引查 (名称, 值)：1~61 查静态表，62+ 查动态表，未命中返回空对。
// 参数：index - HPACK 合并索引
std::pair<std::string_view, std::string_view>
HpackDecoder::Lookup(int index) const
{
    if (index >= 1 && index <= 61)
        return {StaticName(index), StaticValue(index)};

    int dyn_idx = index - 62;
    if (dyn_idx >= 0 && static_cast<size_t>(dyn_idx) < dynamic_table_.size()) {
        auto& e = dynamic_table_[dyn_idx];
        return {e.name, e.value};
    }

    return {};
}

// ═══════════════════════════════════════════════════════════════
// Main Decode
// ═══════════════════════════════════════════════════════════════

// 解码一个 HPACK 头部块：循环处理 5 种表示（索引、增量索引字面量、动态表尺寸更新、
// 永不索引、不索引字面量），将头写入 ctx 并维护动态表。
// 参数：data - HPACK 块数据指针；len - 块长度；ctx - 接收解码结果的消息上下文；
//       返回 true 表示成功，false 表示协议错误
bool HpackDecoder::Decode(const uint8_t* data, size_t len, H2StreamContext& ctx)
{
    while (len > 0) {
        uint8_t byte = data[0];

        if (byte & 0x80) {
            // ── 1. Indexed Header Field (1xxxxxxx) ──
            uint32_t idx = DecodeInteger(data, len, 7);
            if (idx == 0) return false;

            auto [name, value] = Lookup(static_cast<int>(idx));
            if (name.empty()) return false;

            ctx.AddHeader(name, value);

        } else if ((byte & 0xC0) == 0x40) {
            // ── 2. Literal with Incremental Indexing (01xxxxxx) ──
            uint32_t name_idx = DecodeInteger(data, len, 6);

            std::string_view name, value;
            if (name_idx == 0) {
                name = DecodeString(data, len, *ctx.Pool());
                if (name.empty()) return false;
            } else {
                auto nv = Lookup(static_cast<int>(name_idx));
                if (nv.first.empty()) return false;
                name = nv.first;
            }

            value = DecodeString(data, len, *ctx.Pool());
            if (value.data() == nullptr && value.size() > 0) return false;

            ctx.AddHeader(name, value);

            // Add to dynamic table
            auto entry_size = EntryOverhead(name, value);
            EvictTo(max_table_size_ - std::min(entry_size, static_cast<size_t>(max_table_size_)));
            if (entry_size <= max_table_size_) {
                dynamic_table_.emplace_front(
                    std::string{name.data(), name.size()},
                    std::string{value.data(), value.size()});
                current_table_size_ += entry_size;
            }

        } else if ((byte & 0xE0) == 0x20) {
            // ── 3. Dynamic Table Size Update (001xxxxx) ──
            uint32_t new_size = DecodeInteger(data, len, 5);
            if (new_size > max_table_size_) return false;
            EvictTo(new_size);
            max_table_size_ = new_size;

        } else if ((byte & 0xF0) == 0x10) {
            // ── 4. Literal Never Indexed (0001xxxx) ──
            uint32_t name_idx = DecodeInteger(data, len, 4);

            std::string_view name, value;
            if (name_idx == 0) {
                name = DecodeString(data, len, *ctx.Pool());
                if (name.empty()) return false;
            } else {
                auto nv = Lookup(static_cast<int>(name_idx));
                if (nv.first.empty()) return false;
                name = nv.first;
            }

            value = DecodeString(data, len, *ctx.Pool());
            if (value.data() == nullptr && value.size() > 0) return false;

            ctx.AddHeader(name, value);

        } else if ((byte & 0xF0) == 0x00) {
            // ── 5. Literal without Indexing (0000xxxx) ──
            uint32_t name_idx = DecodeInteger(data, len, 4);

            std::string_view name, value;
            if (name_idx == 0) {
                name = DecodeString(data, len, *ctx.Pool());
                if (name.empty()) return false;
            } else {
                auto nv = Lookup(static_cast<int>(name_idx));
                if (nv.first.empty()) return false;
                name = nv.first;
            }

            value = DecodeString(data, len, *ctx.Pool());
            if (value.data() == nullptr && value.size() > 0) return false;

            ctx.AddHeader(name, value);

        } else {
            return false;
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
// HpackEncoder
// ═══════════════════════════════════════════════════════════════

// 在静态表中查找与 (name, value) 完全匹配的条目，返回 1 起始下标，未命中返回 0。
// 参数：name - 头名称；value - 头值
int HpackEncoder::LookupStatic(std::string_view name, std::string_view value) const
{
    for (int i = 0; i < 61; i++) {
        if (name == kStaticNames[i] && value == kStaticValues[i])
            return i + 1;
    }
    return 0;
}

// 在静态表中只按名称查找条目，返回 1 起始下标，未命中返回 0（用于名称索引字面量）。
// 参数：name - 头名称
int HpackEncoder::LookupStaticNameOnly(std::string_view name) const
{
    for (int i = 0; i < 61; i++) {
        if (name == kStaticNames[i])
            return i + 1;
    }
    return 0;
}

// 编码 HPACK 整数（RFC 7541 §5.1）：小于掩码则直接写入前缀位，否则写掩码后跟 7 位续字节。
// 参数：out - 输出字节序列（首字节按前缀位改写）；value - 待编码整数；prefix_bits - 前缀位数
void HpackEncoder::EncodeInteger(std::vector<uint8_t>& out,
                                 uint32_t value, uint8_t prefix_bits)
{
    uint8_t mask = static_cast<uint8_t>((1u << prefix_bits) - 1);

    if (value < mask) {
        if (!out.empty())
            out.back() |= static_cast<uint8_t>(value);
        else
            out.push_back(static_cast<uint8_t>(value));
        return;
    }

    if (!out.empty())
        out.back() |= mask;
    else
        out.push_back(mask);

    value -= mask;
    while (value >= 128) {
        out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

// 编码 HPACK 字符串：非哈夫曼编码（首字节高位 0）+ 7 位前缀长度 + 原始字节（RFC 7541 §5.2）。
// 参数：out - 输出字节序列；str - 待编码字符串
void HpackEncoder::EncodeString(std::vector<uint8_t>& out, std::string_view str)
{
    out.push_back(0);
    EncodeInteger(out, static_cast<uint32_t>(str.size()), 7);
    out.insert(out.end(), str.begin(), str.end());
}

// 将响应头列表编码为 HPACK 块：静态表完全匹配用索引表示，其余用"永不索引字面量"表示。
// 参数：headers - (名称, 值) 头列表；返回 HPACK 编码字节序列
std::vector<uint8_t> HpackEncoder::Encode(
    const std::vector<std::pair<std::string_view, std::string_view>>& headers)
{
    std::vector<uint8_t> result;

    for (auto& [name, value] : headers) {
        int idx = LookupStatic(name, value);
        if (idx > 0) {
            result.push_back(0x80);
            EncodeInteger(result, static_cast<uint32_t>(idx), 7);
            continue;
        }

        idx = LookupStaticNameOnly(name);
        if (idx > 0) {
            result.push_back(0x10);
            EncodeInteger(result, static_cast<uint32_t>(idx), 4);
        } else {
            result.push_back(0x10);
            EncodeInteger(result, 0, 4);
            EncodeString(result, name);
        }

        EncodeString(result, value);
    }

    return result;
}
