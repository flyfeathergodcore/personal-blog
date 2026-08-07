#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class ParseResult { Incomplete = 0, Complete = 1, Error = -1 };

class SessionRegion;   // forward decl

class Context {
public:
    virtual ~Context() = default;

    // Feed raw data into the parser
    virtual ParseResult Feed(const char* data, size_t len) = 0;

    // Access parsed results
    virtual std::string_view Method()  const = 0;
    virtual std::string_view Path()    const = 0;
    virtual std::string_view Version() const = 0;
    virtual std::string_view Header(const std::string_view key) const = 0;
    virtual std::string_view Body()   const = 0;

    // Header enumeration (for proxy forwarding etc.)
    virtual int HeaderCount() const = 0;
    virtual std::pair<std::string_view, std::string_view> HeaderAt(int i) const = 0;

    // Protocol identification (for per-protocol metrics).
    virtual bool IsHttp2() const { return false; }

    // Per-request memory pool (set by Session before Feed).
    SessionRegion* Pool() const { return pool_; }
    void SetPool(SessionRegion* p) const { pool_ = p; }

    // ── Response header injection (middleware → handler) ──
    //
    // Middleware calls AddResponseHeader() before next.Handle().
    // Handler reads injected headers via ResponseHeaders() and
    // writes them to the region.  Cleared at the start of each Feed().
    //
    static constexpr int kMaxExtraHeaders = 8;

    void AddResponseHeader(std::string_view key,
                           std::string_view value) const {
        if (extra_header_count_ >= kMaxExtraHeaders) return;
        extra_header_keys_[extra_header_count_] = key;
        extra_header_vals_[extra_header_count_] = value;
        extra_header_count_++;
    }
    int ResponseHeaderCount() const { return extra_header_count_; }
    std::string_view ResponseHeaderKey(int i) const { return extra_header_keys_[i]; }
    std::string_view ResponseHeaderVal(int i) const { return extra_header_vals_[i]; }
    void ClearResponseHeaders() const {
        extra_header_count_ = 0;
    }

    // ── X-Request-Id ──
    void SetRequestId(std::string_view id) const { request_id_ = id; }
    std::string_view RequestId() const { return request_id_; }

    // ── Path parameters (:id / *) ──
    //
    // Router::Match 捕获的路径参数由 session 在匹配后注入（SetParams），
    // handler 通过 Param("id") 直接取，无需自解析 ctx.Path()。
    // 存储为 string_view：key 指向路由树节点持有的 std::string（稳定），
    // value 指向请求路径缓冲区（SessionRegion），请求期间有效。
    static constexpr int kMaxParams = 8;

    void SetParams(const std::vector<
                   std::pair<std::string_view, std::string_view>>& params) const {
        param_count_ = 0;
        for (auto& [k, v] : params) {
            if (param_count_ >= kMaxParams) break;
            param_names_[param_count_] = k;
            param_vals_[param_count_]  = v;
            param_count_++;
        }
    }
    void ClearParams() const { param_count_ = 0; }
    int  ParamCount() const { return param_count_; }
    std::string_view Param(std::string_view name) const {
        for (int i = 0; i < param_count_; i++)
            if (param_names_[i] == name) return param_vals_[i];
        return {};
    }
    std::pair<std::string_view, std::string_view> ParamAt(int i) const {
        if (i < 0 || i >= param_count_) return {};
        return {param_names_[i], param_vals_[i]};
    }

    // ── Query string (?a=b&c=d) ──
    //
    // 首次访问时从 ctx.Path()（含 query 的完整 URI）惰性解析并缓存。
    // 值不进行 URL 解码（%20 原样），与既有行为一致。
    static constexpr int kMaxQueryPairs = 16;

    std::string_view Query(std::string_view name) const {
        if (!query_parsed_) ParseQuery();
        for (int i = 0; i < query_count_; i++)
            if (query_keys_[i] == name) return query_vals_[i];
        return {};
    }
    int QueryCount() const {
        if (!query_parsed_) ParseQuery();
        return query_count_;
    }
    std::pair<std::string_view, std::string_view> QueryAt(int i) const {
        if (!query_parsed_) ParseQuery();
        if (i < 0 || i >= query_count_) return {};
        return {query_keys_[i], query_vals_[i]};
    }
    void ClearQuery() const { query_parsed_ = false; query_count_ = 0; }

private:
    void ParseQuery() const {
        query_parsed_ = true;
        query_count_  = 0;
        std::string_view path = Path();            // 完整 URI（含 query）
        auto q = path.find('?');
        if (q == std::string_view::npos) return;
        std::string_view qs = path.substr(q + 1);
        std::size_t pos = 0;
        while (pos < qs.size() && query_count_ < kMaxQueryPairs) {
            auto amp = qs.find('&', pos);
            auto seg = (amp == std::string_view::npos)
                ? qs.substr(pos) : qs.substr(pos, amp - pos);
            auto eq = seg.find('=');
            if (eq != std::string_view::npos) {
                query_keys_[query_count_] = seg.substr(0, eq);
                query_vals_[query_count_] = seg.substr(eq + 1);
            } else {                               // key-only（"?flag"）
                query_keys_[query_count_] = seg;
                query_vals_[query_count_] = {};
            }
            query_count_++;
            pos = (amp == std::string_view::npos) ? qs.size() : amp + 1;
        }
    }

    mutable SessionRegion* pool_ = nullptr;

    mutable int extra_header_count_ = 0;
    mutable std::string_view extra_header_keys_[kMaxExtraHeaders];
    mutable std::string_view extra_header_vals_[kMaxExtraHeaders];
    mutable std::string_view request_id_;

    // 路径参数（session 注入）+ query 惰性解析缓存
    mutable std::string_view param_names_[kMaxParams];
    mutable std::string_view param_vals_[kMaxParams];
    mutable int param_count_ = 0;
    mutable std::string_view query_keys_[kMaxQueryPairs];
    mutable std::string_view query_vals_[kMaxQueryPairs];
    mutable int query_count_ = 0;
    mutable bool query_parsed_ = false;
};
