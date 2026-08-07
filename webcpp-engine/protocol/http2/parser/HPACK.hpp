#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <string>
#include <vector>
#include <deque>
#include <utility>

class H2StreamContext;
class SessionRegion;

// ═══════════════════════════════════════════════════════════════
// HPACK — RFC 7541 Header Compression
//
// Pure functions for HTTP/2 header compression encoding and decoding.
// ──
// HpackDecoder  — Decode HPACK blocks from incoming HEADERS frames.
//                 Manages the dynamic table and handles Huffman decoding.
// HpackEncoder  — Encode response headers into HPACK blocks.
//                 Uses static table indexing + literal never-indexed.
// ═══════════════════════════════════════════════════════════════

class HpackDecoder {
public:
    // 默认构造函数。
    HpackDecoder() = default;

    /// 设置动态表最大尺寸（来自对端 SETTINGS HEADER_TABLE_SIZE）。
    void SetMaxTableSize(uint32_t size);

    /// 解码 HPACK 头部块；每解出一个头调用 ctx.AddHeader()。
    /// 成功返回 true，协议错误返回 false。
    bool Decode(const uint8_t* data, size_t len, H2StreamContext& ctx);

private:
    // ── Static/dynamic table data ──
    // 显式构造函数：emplace_front(name, value) 跨编译器可靠
    //（clang + libstdc++ 11 的 construct_at 不隐式做聚合括号构造）
    struct HeaderField {
        // 默认构造函数。
        HeaderField() = default;
        // 以名称/值构造动态表条目。
        HeaderField(std::string n, std::string v) : name(std::move(n)), value(std::move(v)) {}
        std::string name;
        std::string value;
    };

    // 按 1 起始下标查静态表名称/值。
    static std::string_view StaticName(int index);   // 1-based
    static std::string_view StaticValue(int index);  // 1-based

    std::deque<HeaderField> dynamic_table_;
    uint32_t max_table_size_   = 4096;
    uint32_t current_table_size_ = 0;

    // ── Decode helpers ──
    // 解码 HPACK 整数（RFC 7541 §5.1）。
    uint32_t DecodeInteger(const uint8_t*& data, size_t& len, uint8_t prefix_bits);
    // 解码 HPACK 字符串（RFC 7541 §5.2），结果拷贝到会话区域。
    std::string_view DecodeString(const uint8_t*& data, size_t& len,
                                   SessionRegion& region);
    // 哈夫曼解码（RFC 7541 §5.2）。
    std::string HuffmanDecode(const uint8_t* data, size_t len);

    // ── Dynamic table helpers ──
    // 计算动态表条目占用的表空间（32 + 名称长度 + 值长度）。
    static size_t EntryOverhead(std::string_view n, std::string_view v);
    // 淘汰动态表条目直到表大小不超过目标值。
    void EvictTo(uint32_t target_size);

    // ── Name/value lookup by combined index ──
    // 按合并索引查 (名称, 值)，未命中返回空对。
    std::pair<std::string_view, std::string_view> Lookup(int index) const;
};


class HpackEncoder {
public:
    // 默认构造函数。
    HpackEncoder() = default;

    /// 将响应头编码为 HPACK 格式：常用头用静态表索引表示，其余用"永不索引字面量"。
    std::vector<uint8_t> Encode(
        const std::vector<std::pair<std::string_view, std::string_view>>& headers);

private:
    // 编码 HPACK 整数（RFC 7541 §5.1）。
    void EncodeInteger(std::vector<uint8_t>& out, uint32_t value, uint8_t prefix_bits);
    // 编码 HPACK 字符串（非哈夫曼，RFC 7541 §5.2）。
    void EncodeString(std::vector<uint8_t>& out, std::string_view str);

    /// 在静态表中查 (name, value) 完全匹配项，返回 1 起始下标或 0。
    int LookupStatic(std::string_view name, std::string_view value) const;
    /// 在静态表中只按名称查，返回 1 起始下标或 0。
    int LookupStaticNameOnly(std::string_view name) const;
};
