#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class ParseResult { Incomplete = 0, Complete = 1, Error = -1 };

class SessionRegion;   // forward decl

class Context {
public:
    // 虚析构（多态基类）
    virtual ~Context() = default;

    // 向解析器喂入原始数据，推进解析状态机
    // 参数：data - 输入数据指针；len - 输入长度（字节）；返回：解析结果
    virtual ParseResult Feed(const char* data, size_t len) = 0;

    // 获取解析出的请求方法（如 GET/POST）
    // 参数：无
    virtual std::string_view Method()  const = 0;
    // 获取解析出的请求路径（含 query 串）
    // 参数：无
    virtual std::string_view Path()    const = 0;
    // 获取解析出的 HTTP 版本（如 HTTP/1.1）
    // 参数：无
    virtual std::string_view Version() const = 0;
    // 按名称查询单个请求头（大小写不敏感）
    // 参数：key - 头部名称；返回：值，未找到返回空
    virtual std::string_view Header(const std::string_view key) const = 0;
    // 获取请求体
    // 参数：无
    virtual std::string_view Body()   const = 0;

    // 获取请求头总数（用于代理转发等枚举场景）
    // 参数：无
    virtual int HeaderCount() const = 0;
    // 获取第 i 个请求头（名称, 值）对
    // 参数：i - 头部索引；返回：越界返回空对
    virtual std::pair<std::string_view, std::string_view> HeaderAt(int i) const = 0;

    // 协议识别：是否为 HTTP/2（用于按协议统计）
    // 参数：无
    virtual bool IsHttp2() const { return false; }

    // 获取请求级内存池（Session 在 Feed 前设置）
    // 参数：无
    SessionRegion* Pool() const { return pool_; }
    // 设置请求级内存池
    // 参数：p - 区域池指针
    void SetPool(SessionRegion* p) const { pool_ = p; }

    // ── Response header injection (middleware → handler) ──
    //
    // Middleware calls AddResponseHeader() before next.Handle().
    // Handler reads injected headers via ResponseHeaders() and
    // writes them to the region.  Cleared at the start of each Feed().
    //
    static constexpr int kMaxExtraHeaders = 8;

    // 注入一个额外响应头（middleware 在 next.Handle() 前调用，数量上限 kMaxExtraHeaders）
    // 参数：key - 头部名称；value - 头部值
    void AddResponseHeader(std::string_view key,
                           std::string_view value) const {
        if (extra_header_count_ >= kMaxExtraHeaders) return;
        extra_header_keys_[extra_header_count_] = key;
        extra_header_vals_[extra_header_count_] = value;
        extra_header_count_++;
    }
    // 获取已注入的额外响应头数量
    // 参数：无
    int ResponseHeaderCount() const { return extra_header_count_; }
    // 获取第 i 个注入响应头的名称
    // 参数：i - 索引
    std::string_view ResponseHeaderKey(int i) const { return extra_header_keys_[i]; }
    // 获取第 i 个注入响应头的值
    // 参数：i - 索引
    std::string_view ResponseHeaderVal(int i) const { return extra_header_vals_[i]; }
    // 清空已注入的响应头（每次 Feed 开始时调用）
    // 参数：无
    void ClearResponseHeaders() const {
        extra_header_count_ = 0;
    }

    // ── X-Request-Id ──
    // 设置请求追踪 ID（X-Request-Id）
    // 参数：id - 请求 ID
    void SetRequestId(std::string_view id) const { request_id_ = id; }
    // 获取请求追踪 ID
    // 参数：无
    std::string_view RequestId() const { return request_id_; }

    // ── Path parameters (:id / *) ──
    //
    // Router::Match 捕获的路径参数由 session 在匹配后注入（SetParams），
    // handler 通过 Param("id") 直接取，无需自解析 ctx.Path()。
    // 存储为 string_view：key 指向路由树节点持有的 std::string（稳定），
    // value 指向请求路径缓冲区（SessionRegion），请求期间有效。
    static constexpr int kMaxParams = 8;

    // 注入路由匹配的路径参数（session 在 Router::Match 后调用）
    // 参数：params - {参数名, 参数值} 列表
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
    // 清空路径参数
    // 参数：无
    void ClearParams() const { param_count_ = 0; }
    // 获取路径参数个数
    // 参数：无
    int  ParamCount() const { return param_count_; }
    // 按名称获取路径参数值
    // 参数：name - 参数名；返回：值，未找到返回空
    std::string_view Param(std::string_view name) const {
        for (int i = 0; i < param_count_; i++)
            if (param_names_[i] == name) return param_vals_[i];
        return {};
    }
    // 获取第 i 个路径参数（名称, 值）对
    // 参数：i - 索引；返回：越界返回空对
    std::pair<std::string_view, std::string_view> ParamAt(int i) const {
        if (i < 0 || i >= param_count_) return {};
        return {param_names_[i], param_vals_[i]};
    }

    // ── Query string (?a=b&c=d) ──
    //
    // 首次访问时从 ctx.Path()（含 query 的完整 URI）惰性解析并缓存。
    // 值不进行 URL 解码（%20 原样），与既有行为一致。
    static constexpr int kMaxQueryPairs = 16;

    // 按名称查询 query 参数（首次访问惰性解析并缓存，不做 URL 解码）
    // 参数：name - 参数名；返回：值，未找到返回空
    std::string_view Query(std::string_view name) const {
        if (!query_parsed_) ParseQuery();
        for (int i = 0; i < query_count_; i++)
            if (query_keys_[i] == name) return query_vals_[i];
        return {};
    }
    // 获取 query 参数个数（惰性解析）
    // 参数：无
    int QueryCount() const {
        if (!query_parsed_) ParseQuery();
        return query_count_;
    }
    // 获取第 i 个 query 参数（名称, 值）对
    // 参数：i - 索引；返回：越界返回空对
    std::pair<std::string_view, std::string_view> QueryAt(int i) const {
        if (!query_parsed_) ParseQuery();
        if (i < 0 || i >= query_count_) return {};
        return {query_keys_[i], query_vals_[i]};
    }
    // 清空 query 缓存（每请求重建）
    // 参数：无
    void ClearQuery() const { query_parsed_ = false; query_count_ = 0; }

private:
    // 惰性解析 query 串并缓存到数组（仅解析一次）
    // 参数：无
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
