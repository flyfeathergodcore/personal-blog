// ═══════════════════════════════════════════════════════════════════
// mysql_handler — 博客后端 REST API 实现
//
// 数据流：HTTP 请求 → HandleAsync（协程）→ thread_local 连接池借连接
//        → async_query / async_update → 释放连接 → JSON 响应
//
// 设计要点：
//   - 每 worker 线程一个连接池（thread_local 裸指针，懒创建），对应每个
//     worker 独立 EventLoop 的要求；进程退出由 OS 回收，避免线程退出时
//     thread_local 析构与 EventLoop 析构的时序问题。
//   - 连接借用用 RAII（ConnGuard）保证异常安全归还。
//   - async_query 返回的 MYSQL_RES* 由调用方 mysql_free_result。
//   - id 一律按字符串序列化（前端 Article.id 等为 string 类型）。
//   - tags 在库内以逗号分隔字符串存储，序列化时转 JSON 数组。
// ═══════════════════════════════════════════════════════════════════
#include "handler/mysql_handler.hpp"
#include "handler/metrics.hpp"

#include "log/logger.hpp"
#include "protocol/context.hpp"
#include "protocol/response.hpp"
#include "protocol/session_region.hpp"
#include "router/router.hpp"
#include "coro/task.h"
#include "coro/awaiter.h"

#include "connectionpool.h"
#include <mysql/mysql.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// ═══════════════════════════════════════════════════════════════════
// 局域网访问开关（后台「工作区 → 局域网访问」）
//
//   - g_lan_enabled：默认关闭（更安全）。关闭时 LanGuardMiddleware 拒绝
//     局域网 IP Host 的请求（本机 localhost 放行），设备访问看到 403。
//   - g_lan_ip：宿主机局域网 IP，由 build-run.sh 注入环境变量 HOST_LAN_IP。
//   - 开关状态落库 site_config 表（CREATE TABLE IF NOT EXISTS），容器重建后保持。
// ═══════════════════════════════════════════════════════════════════
static std::atomic<bool> g_lan_enabled{false};
static std::string g_lan_ip;

// ═══════════════════════════════════════════════════════════════════
// 访问者统计（宿主机转发器 lan-proxy.py 上报连接方真实 IP）
//
//   转发器在宿主机监听 0.0.0.0:8443（对外统一入口），每个连接取真实对端
//   IP（本机 127.0.0.1 / 局域网 192.168.x.x），连接建立/断开时 POST
//   /api/network/visitor 上报；这里用内存维护「当前在线」状态，
//   累计访问次数与最近访问时间落库 visitor_stats 表。
// ═══════════════════════════════════════════════════════════════════
// 当前在线连接数（IP → 活动连接数，连接建立 +1、断开 -1）
static std::mutex g_visitor_mutex;
static std::map<std::string, int> g_active_visitors;

namespace {

// ═══════════════════════════════════════════════════════════════════
// JSON 工具
// ═══════════════════════════════════════════════════════════════════

// 写入 JSON 字符串转义（UTF-8 原样透传；控制字符 \uXXXX；引号/反斜杠/换行转义）
void JsonEscape(std::string& out, std::string_view s) {
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
}

// 返回一个 JSON 字符串字面量（含引号）
std::string JsonStr(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    JsonEscape(o, s);
    o += '"';
    return o;
}

// 从 JSON 对象 body 中提取 "key":"value" 的 value（已解转义）。
// 支持 JSON 转义（\" \\ \/ \b \f \n \r \t \uXXXX）——前端 JSON.stringify
// 会把 content 里的换行/引号转义，必须正确处理，否则多行内容提取错位。
std::string JsonField(std::string_view body, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto p = body.find(needle);
    if (p == std::string_view::npos) return {};
    auto colon = body.find(':', p + needle.size());
    if (colon == std::string_view::npos) return {};
    auto i = body.find('"', colon);
    if (i == std::string_view::npos) return {};
    ++i;
    std::string out;
    while (i < body.size()) {
        char c = body[i];
        if (c == '"') return out;            // 未转义的结束引号
        if (c == '\\') {
            ++i;
            if (i >= body.size()) break;
            char e = body[i];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {  // \uXXXX → UTF-8（BMP；代理对场景前端极少用，容错略过）
                    if (i + 4 < body.size()) {
                        unsigned code = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; ++k) {
                            char h = body[i + 1 + k];
                            code <<= 4;
                            if (h >= '0' && h <= '9')       code |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f')  code |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')  code |= static_cast<unsigned>(h - 'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok) {
                            i += 4;
                            if (code < 0x80) {
                                out += static_cast<char>(code);
                            } else if (code < 0x800) {
                                out += static_cast<char>(0xC0 | (code >> 6));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (code >> 12));
                                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            }
                        } else {
                            out += 'u';
                        }
                    } else {
                        out += 'u';
                    }
                    break;
                }
                default: out += e; break;
            }
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    return out;   // 未闭合引号也返回已收集内容（容错）
}

// 从 JSON 对象 body 中提取数组字段 ["a","b"] → 逗号分隔字符串 "a,b"（存库用）
std::string JsonArrayToCsv(std::string_view body, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto p = body.find(needle);
    if (p == std::string_view::npos) return {};
    auto colon = body.find(':', p + needle.size());
    if (colon == std::string_view::npos) return {};
    auto open = body.find('[', colon);
    if (open == std::string_view::npos) return {};
    auto close = body.find(']', open);
    if (close == std::string_view::npos) return {};
    std::string_view rest = body.substr(open + 1, close - open - 1);
    std::string out;
    bool first = true;
    while (!rest.empty()) {
        auto i = rest.find('"');
        if (i == std::string_view::npos) break;
        auto j = rest.find('"', i + 1);
        if (j == std::string_view::npos) break;
        if (!first) out += ',';
        out += std::string(rest.substr(i + 1, j - i - 1));
        first = false;
        rest = rest.substr(j + 1);
    }
    return out;
}

// 逗号分隔字符串 → JSON 数组（["a","b"]）
std::string TagsToJson(std::string_view tags) {
    std::string o = "[";
    bool first = true;
    std::string_view rest = tags;
    while (!rest.empty()) {
        auto comma = rest.find(',');
        auto tag = (comma == std::string_view::npos)
                       ? rest : rest.substr(0, comma);
        if (!first) o += ',';
        o += JsonStr(tag);
        first = false;
        rest = (comma == std::string_view::npos)
                   ? std::string_view{} : rest.substr(comma + 1);
    }
    o += ']';
    return o;
}

// 百分号解码（query 参数中文字符会被浏览器/前端 encodeURIComponent）
std::string PercentDecode(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]);
            int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                o += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

// ═══════════════════════════════════════════════════════════════════
// MySQL 异步连接池（每 worker 线程一个）
// ═══════════════════════════════════════════════════════════════════

// 懒创建 + 进程退出 OS 回收：避免 worker 线程退出时 thread_local 析构
// 与 EventLoop 析构的先后问题（connectionpool::close 内部会调 current()）。
thread_local connectionpool* t_pool = nullptr;

connectionpool* GetPool(const MysqlConfig& cfg) {
    if (!t_pool) {
        connectionpool::Config pc;
        pc.host = cfg.host.c_str();
        pc.user = cfg.user.c_str();
        pc.password = cfg.password.c_str();
        pc.database = cfg.database.c_str();
        pc.min_size = cfg.min_size;
        pc.max_size = cfg.max_size;
        t_pool = new connectionpool(pc);
    }
    return t_pool;
}

// 借用 RAII guard：析构时自动归还连接（异常安全）
class ConnGuard {
public:
    explicit ConnGuard(connectionpool* pool) : pool_(pool) {}
    ~ConnGuard() { if (c_) pool_->release(c_); }
    ConnGuard(const ConnGuard&) = delete;
    ConnGuard& operator=(const ConnGuard&) = delete;

    // 在 HandleAsync 协程内调用；borrow 本身是协程，返回 coro::Task<bool>
    //（vue-web/coro 的 Task 经 operator co_await 可直接 co_await）
    coro::Task<bool> borrow() {
        c_ = co_await coro::AwaitTask<connection*>{pool_->async_borrow()};
        co_return c_ != nullptr;
    }
    connection* get() const { return c_; }

private:
    connectionpool* pool_;
    connection* c_ = nullptr;
};

// SQL 字符串转义（用 mysql_real_escape_string，按当前 session 的 sql_mode 正确处理反斜杠）
std::string SqlEscape(MYSQL* conn, std::string_view s) {
    if (!conn) return std::string(s);
    std::string o(2 * s.size() + 1, '\0');
    unsigned long len = mysql_real_escape_string(conn, o.data(), s.data(),
                                                 static_cast<unsigned long>(s.size()));
    o.resize(len);
    return o;
}

// ═══════════════════════════════════════════════════════════════════
// 上传：multipart/form-data 解析 + base64
// ═══════════════════════════════════════════════════════════════════

struct MultipartFile {
    std::string filename;
    std::string mime;
    std::string data;
};

// 解析 multipart/form-data body，取出第一个文件字段（filename / mime / 二进制）
MultipartFile ParseMultipart(std::string_view body, std::string_view content_type) {
    MultipartFile f;
    auto bp = content_type.find("boundary=");
    if (bp == std::string_view::npos) return f;
    std::string_view bv = content_type.substr(bp + 9);
    // 去掉可选引号 boundary="xxx"
    if (!bv.empty() && bv.front() == '"') {
        bv.remove_prefix(1);
        if (!bv.empty() && bv.back() == '"') bv.remove_suffix(1);
    }
    const std::string boundary = "--" + std::string(bv);

    auto pos = body.find(boundary);
    if (pos == std::string_view::npos) return f;
    pos += boundary.size();
    if (body.substr(pos, 2) == "--") return f;          // 结束标记
    if (body.substr(pos, 2) == "\r\n") pos += 2;

    auto hdr_end = body.find("\r\n\r\n", pos);
    if (hdr_end == std::string_view::npos) return f;
    std::string_view hdr = body.substr(pos, hdr_end - pos);

    // filename="xxx"
    auto fn = hdr.find("filename=\"");
    if (fn != std::string_view::npos) {
        auto s = hdr.find('"', fn);      // 起始引号
        auto e = hdr.find('"', s + 1);   // 结束引号
        if (s != std::string_view::npos && e != std::string_view::npos)
            f.filename = std::string(hdr.substr(s + 1, e - s - 1));
    }
    // Content-Type: xxx
    auto ct = hdr.find("Content-Type:");
    if (ct != std::string_view::npos) {
        auto e = hdr.find("\r\n", ct);
        std::string_view val = hdr.substr(
            ct + 13, (e == std::string_view::npos ? hdr.size() : e) - (ct + 13));
        while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
        f.mime = std::string(val);
    }

    // 文件二进制：头部空行之后，到下一个 --boundary 之前
    auto data_start = hdr_end + 4;
    auto data_end = body.find("\r\n" + boundary, data_start);
    if (data_end == std::string_view::npos) data_end = body.size();
    f.data = std::string(body.substr(data_start, data_end - data_start));
    return f;
}

// base64 编码（图片转 data URL 存库）
std::string Base64Encode(const std::string& in) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                     (static_cast<uint8_t>(in[i + 1]) << 8) |
                     static_cast<uint8_t>(in[i + 2]);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += kTable[n & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t n = static_cast<uint8_t>(in[i]) << 16;
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                     (static_cast<uint8_t>(in[i + 1]) << 8);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// base64 解码（用于把 data URL 还原成二进制文件，供图片/视频按 id 读取）
std::string Base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;             // 补齐位到此为止
        int v = val(c);
        if (v < 0) continue;             // 忽略空白等无关字符
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xff);
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════
// MySQL 结果集 → JSON
// ═══════════════════════════════════════════════════════════════════

// 按固定列序（SELECT *）：0 id,1 title,2 summary,3 category,4 tags,
// 5 date,6 cover,7 author,8 content,9 file_url
std::string BuildArticleJson(MYSQL_ROW row) {
    auto cell = [&](unsigned int i) -> std::string_view {
        return (row[i]) ? std::string_view(row[i]) : std::string_view{};
    };
    std::string o = "{";
    o += "\"id\":" + JsonStr(cell(0));
    o += ",\"title\":" + JsonStr(cell(1));
    o += ",\"summary\":" + JsonStr(cell(2));
    o += ",\"category\":" + JsonStr(cell(3));
    o += ",\"tags\":" + TagsToJson(cell(4));
    o += ",\"date\":" + JsonStr(cell(5));
    o += ",\"cover\":" + JsonStr(cell(6));
    o += ",\"author\":" + JsonStr(cell(7));
    o += ",\"content\":" + JsonStr(cell(8));
    o += ",\"fileUrl\":" + JsonStr(cell(9));
    o += "}";
    return o;
}

// 文章列表 JSON（可能为空 → []）
std::string BuildArticlesJson(MYSQL_RES* res) {
    std::string o = "[";
    bool first = true;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!first) o += ',';
        o += BuildArticleJson(row);
        first = false;
    }
    o += ']';
    return o;
}

// 分类列表 JSON
std::string BuildCategoriesJson(MYSQL_RES* res) {
    std::string o = "[";
    bool first = true;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!first) o += ',';
        o += "{\"id\":" + JsonStr(row[0] ? row[0] : "");
        o += ",\"name\":" + JsonStr(row[1] ? row[1] : "") + "}";
        first = false;
    }
    o += ']';
    return o;
}

// 资源列表 JSON
// 资源列表 JSON（只返回元数据 id/name/type，不含 url——大文件是 data URL，
// 全量下发会让列表接口 body 巨大导致卡死；url 按 id 走 /api/resources/:id 按需取）
std::string BuildResourcesJson(MYSQL_RES* res) {
    std::string o = "[";
    bool first = true;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!first) o += ',';
        o += "{\"id\":" + JsonStr(row[0] ? row[0] : "");
        o += ",\"name\":" + JsonStr(row[1] ? row[1] : "");
        o += ",\"type\":" + JsonStr(row[3] ? row[3] : "") + "}";
        first = false;
    }
    o += ']';
    return o;
}

// 用户列表 JSON（只暴露 id/username/created_at，绝不下发 password）
std::string BuildUsersJson(MYSQL_RES* res) {
    std::string o = "[";
    bool first = true;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!first) o += ',';
        o += "{\"id\":" + JsonStr(row[0] ? row[0] : "");
        o += ",\"username\":" + JsonStr(row[1] ? row[1] : "");
        o += ",\"createdAt\":" + JsonStr(row[2] ? row[2] : "") + "}";
        first = false;
    }
    o += ']';
    return o;
}

// ═══════════════════════════════════════════════════════════════════
// Handler 基类
// ═══════════════════════════════════════════════════════════════════

class MysqlHandlerBase : public RequestHandler {
public:
    explicit MysqlHandlerBase(const MysqlConfig& cfg) : cfg_(cfg) {}

    // 同步兜底：异步路径 handler 的真实请求走 HandleAsync，不会落到这里
    Response Handle(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) return Response::Raw(500, "no pool");
        return Response::Error(405, *pool);
    }

    bool IsAsync() const override { return true; }

protected:
    // JSON 响应工厂（region 版写法，照抄 routes_demo）
    static Response JsonResponse(SessionRegion* pool, int code,
                                 std::string_view body) {
        if (!pool) return Response::Raw(code, "no pool");
        Response resp(code, *pool);
        resp.Header("Content-Type", "application/json");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write({body.data(), body.size()});
        return resp;
    }

    const MysqlConfig& cfg_;
};

// ═══════════════════════════════════════════════════════════════════
// 具体 Handler
// ═══════════════════════════════════════════════════════════════════

// POST /api/login {username,password} → {token,username} | 401
class LoginHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string body(ctx.Body());
        const std::string username = JsonField(body, "username");
        const std::string password = JsonField(body, "password");
        if (username.empty() || password.empty())
            co_return JsonResponse(pool, 400, R"({"message":"缺少用户名或密码"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();

        std::string sql = "SELECT id FROM users WHERE username='";
        sql += SqlEscape(raw, username);
        sql += "' AND password='";
        sql += SqlEscape(raw, password);
        sql += "' LIMIT 1";
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        bool found = (res && mysql_num_rows(res) > 0);
        if (res) mysql_free_result(res);
        if (!found)
            co_return JsonResponse(pool, 401, R"({"message":"用户名或密码错误"})");

        // 签发 token（demo 用，服务端不校验，仅前端守卫判断存在性）
        std::string token = "tk-" + username + "-" +
            std::to_string(static_cast<long long>(std::time(nullptr)));
        std::string resp_body = "{\"token\":" + JsonStr(token) +
                                ",\"username\":" + JsonStr(username) + "}";
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// GET /api/articles?category=xxx → 文章数组
class ArticlesHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");

        std::string sql = "SELECT * FROM articles";
        auto cat = ctx.Query("category");
        if (!cat.empty()) {
            sql += " WHERE category='";
            sql += SqlEscape(nullptr, PercentDecode(cat));
            sql += "'";
        }
        sql += " ORDER BY id DESC";

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string resp_body = BuildArticlesJson(res);
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// GET /api/articles/:id → 单篇文章 | 404
class ArticleByIdHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return JsonResponse(pool, 400, R"({"message":"缺少文章 id"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "SELECT * FROM articles WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "' LIMIT 1";
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
        std::string resp_body;
        if (row) {
            resp_body = BuildArticleJson(row);
        } else {
            resp_body = R"({"message":"文章不存在"})";
        }
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, row ? 200 : 404, resp_body);
    }
};

// POST /api/articles（upsert：body 含 id → UPDATE，否则 INSERT）
class SaveArticleHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string body(ctx.Body());
        const std::string id = JsonField(body, "id");
        const std::string title = JsonField(body, "title");
        const std::string summary = JsonField(body, "summary");
        const std::string category = JsonField(body, "category");
        const std::string tags = JsonArrayToCsv(body, "tags");
        const std::string date = JsonField(body, "date");
        const std::string cover = JsonField(body, "cover");
        const std::string author = JsonField(body, "author");
        const std::string content = JsonField(body, "content");
        const std::string file_url = JsonField(body, "fileUrl");
        if (title.empty())
            co_return JsonResponse(pool, 400, R"({"message":"标题不能为空"})");

        // 文章 id 是 INT AUTO_INCREMENT（上限 2147483647）：仅接受合法正整数。
        // 前端新增时若传 Date.now() 时间戳（13 位）会超 INT 范围导致 500，
        // 这里统一过滤：非法 id 一律按新增（NULL 走 AUTO_INCREMENT）处理。
        auto is_valid_id = [](const std::string& s) {
            if (s.empty()) return false;
            long long v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
                if (v > 2147483647LL) return false;
            }
            return v >= 1;
        };
        const bool has_valid_id = is_valid_id(id);

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();
        auto esc = [raw](const std::string& s) { return SqlEscape(raw, s); };

        std::string sql;
        bool is_update = false;
        if (has_valid_id) {
            // 合法 id：先查库，存在才 UPDATE，不存在则 INSERT 复用该 id
            std::string check = "SELECT id FROM articles WHERE id='" + esc(id) + "' LIMIT 1";
            MYSQL_RES* chk = nullptr;
            try {
                chk = co_await coro::AwaitTask<MYSQL_RES*>{
                    g.get()->async_query(check.c_str(), 5000)};
            } catch (const std::exception& e) {
                if (chk) mysql_free_result(chk);
                co_return JsonResponse(pool, 500,
                    "{\"message\":" + JsonStr(e.what()) + "}");
            }
            bool exists = (chk && mysql_num_rows(chk) > 0);
            if (chk) mysql_free_result(chk);
            is_update = exists;
        }
        if (is_update) {
            sql = "UPDATE articles SET title='" + esc(title) +
                  "',summary='" + esc(summary) +
                  "',category='" + esc(category) +
                  "',tags='" + esc(tags) +
                  "',date='" + esc(date) +
                  "',cover='" + esc(cover) +
                  "',author='" + esc(author) +
                  "',content='" + esc(content) +
                  "',file_url='" + esc(file_url) +
                  "' WHERE id='" + esc(id) + "'";
        } else {
            // 合法 id 显式写入，非法/为空则 NULL 走 AUTO_INCREMENT
            sql = "INSERT INTO articles(id,title,summary,category,tags,date,cover,author,content,file_url) VALUES(" +
                  (has_valid_id ? "'" + esc(id) + "'" : std::string("NULL")) +
                  ",'" + esc(title) + "','" + esc(summary) + "','" + esc(category) +
                  "','" + esc(tags) + "','" + esc(date) + "','" + esc(cover) +
                  "','" + esc(author) + "','" + esc(content) + "','" +
                  esc(file_url) + "')";
        }
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"保存成功"})");
    }
};

// DELETE /api/articles/:id
class DeleteArticleHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return JsonResponse(pool, 400, R"({"message":"缺少文章 id"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "DELETE FROM articles WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "'";
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"删除成功"})");
    }
};

// GET /api/categories → 分类数组
class CategoriesHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query("SELECT id,name FROM categories ORDER BY id", 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string resp_body = BuildCategoriesJson(res);
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// POST /api/categories（upsert：body 含 id → UPDATE name，否则 INSERT）
class SaveCategoryHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string body(ctx.Body());
        const std::string id = JsonField(body, "id");
        const std::string name = JsonField(body, "name");
        if (name.empty())
            co_return JsonResponse(pool, 400, R"({"message":"分类名不能为空"})");

        // 分类 id 是 INT：非法 id（如前端 Date.now() 时间戳）一律按新增处理，
        // 否则会走 UPDATE 但匹配不到行 → 返回成功却没有真正添加
        auto is_valid_id = [](const std::string& s) {
            if (s.empty()) return false;
            long long v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
                if (v > 2147483647LL) return false;
            }
            return v >= 1;
        };
        const bool has_valid_id = is_valid_id(id);

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();
        std::string sql;
        if (has_valid_id) {
            sql = "UPDATE categories SET name='" + SqlEscape(raw, name) +
                  "' WHERE id='" + SqlEscape(raw, id) + "'";
        } else {
            sql = "INSERT INTO categories(name) VALUES('" + SqlEscape(raw, name) + "')";
        }
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"保存成功"})");
    }
};

// DELETE /api/categories/:id
class DeleteCategoryHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return JsonResponse(pool, 400, R"({"message":"缺少分类 id"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "DELETE FROM categories WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "'";
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"删除成功"})");
    }
};

// GET /api/resources → 资源数组
class ResourcesHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query("SELECT id,name,url,type FROM resources ORDER BY id DESC", 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string resp_body = BuildResourcesJson(res);
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// GET /api/resources/:id → {id,name,url,type}（含完整 data URL，供复制地址/选背景等按需取）
class ResourceByIdHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return JsonResponse(pool, 400, R"({"message":"缺少资源 id"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "SELECT id,name,url,type FROM resources WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "'";
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
        if (!row) {
            if (res) mysql_free_result(res);
            co_return JsonResponse(pool, 404, R"({"message":"资源不存在"})");
        }
        std::string body = "{\"id\":" + JsonStr(row[0] ? row[0] : "");
        body += ",\"name\":" + JsonStr(row[1] ? row[1] : "");
        body += ",\"url\":" + JsonStr(row[2] ? row[2] : "");
        body += ",\"type\":" + JsonStr(row[3] ? row[3] : "") + "}";
        mysql_free_result(res);
        co_return JsonResponse(pool, 200, body);
    }
};

// GET /api/img/:id → 把库里的 data URL 解码成文件二进制返回（外链则 302），
// 供 <img>/<video> 直接引用；列表接口因此无需全量下发大文件。
class ResourceImageHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return Response::Raw(400, "missing id");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return Response::Raw(500, "db fail");
        std::string sql = "SELECT url FROM resources WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "'";
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string url;
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0]) url = row[0];
            mysql_free_result(res);
        }
        if (url.empty()) co_return Response::Raw(404, "not found");

        // 外链图片/视频（种子资源是 picsum 等 http(s) 地址）：302 重定向，浏览器跟随加载
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
            Response redirect(302, *pool);
            redirect.Header("Location", url);
            redirect.EndHeaders();
            co_return redirect;
        }

        // 解析 data URL：data:<mime>;base64,<b64>
        const std::string kPrefix = "data:";
        if (url.rfind(kPrefix, 0) != 0) co_return Response::Raw(400, "not a data url");
        auto semi = url.find(';', kPrefix.size());
        auto comma = url.find(',', kPrefix.size());
        if (semi == std::string::npos || comma == std::string::npos)
            co_return Response::Raw(400, "bad data url");
        const std::string mime = url.substr(kPrefix.size(), semi - kPrefix.size());
        std::string bin = Base64Decode(url.substr(comma + 1));

        // 二进制响应：Content-Type 取 data URL 里的 mime，body 为解码后文件
        Response resp(200, *pool);
        resp.Header("Content-Type", mime.empty() ? "application/octet-stream" : mime);
        resp.Header("Content-Length", bin.size());
        resp.EndHeaders();
        pool->Write({bin.data(), bin.size()});
        co_return resp;
    }
};

// DELETE /api/resources/:id
class DeleteResourceHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return JsonResponse(pool, 400, R"({"message":"缺少资源 id"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "DELETE FROM resources WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "'";
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"删除成功"})");
    }
};

// POST /api/upload/image（multipart）→ {url: dataUrl}；文件入库为 data URL
class UploadImageHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string ct(ctx.Header("content-type"));
        const std::string body(ctx.Body());
        MultipartFile f = ParseMultipart(body, ct);
        if (f.filename.empty())
            co_return JsonResponse(pool, 400, R"({"message":"未找到上传文件"})");

        const std::string data_url = "data:" +
            (f.mime.empty() ? std::string("application/octet-stream") : f.mime) +
            ";base64," + Base64Encode(f.data);

        // 按 mime 或扩展名判断资源类型：视频归 video，其余（图片等）归 image，
        // 资源管理据此用 video 标签 / el-image 正确渲染
        auto to_lower = [](std::string s) {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        auto is_video = [&](const std::string& mime, const std::string& name) {
            if (mime.rfind("video/", 0) == 0) return true;
            auto dot = name.rfind('.');
            if (dot == std::string::npos) return false;
            const std::string ext = to_lower(name.substr(dot + 1));
            return ext == "mp4" || ext == "webm" || ext == "mov" ||
                   ext == "mkv" || ext == "m4v" || ext == "ogv";
        };
        const std::string res_type =
            is_video(f.mime, f.filename) ? "video" : "image";

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();
        std::string sql = "INSERT INTO resources(name,url,type) VALUES('";
        sql += SqlEscape(raw, f.filename) + "','";
        sql += SqlEscape(raw, data_url) + "','" + res_type + "')";
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string resp_body = "{\"url\":" + JsonStr(data_url) + "}";
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// POST /api/upload/md（multipart）→ {url: dataUrl, name}；Markdown 文件
// base64 成 data URL 入库 resources（type='md'），与图片上传架构一致。
// 前端详情页 MdPage fetch(dataUrl) 即可直接取回原始文本渲染。
class UploadMdHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string ct(ctx.Header("content-type"));
        const std::string body(ctx.Body());
        MultipartFile f = ParseMultipart(body, ct);
        if (f.filename.empty())
            co_return JsonResponse(pool, 400, R"({"message":"未找到上传文件"})");

        // 只接受 Markdown 文本（按扩展名过滤，中文文件名不受影响）
        auto is_md = [](const std::string& name) {
            auto dot = name.rfind('.');
            if (dot == std::string::npos) return false;
            std::string ext = name.substr(dot + 1);
            return ext == "md" || ext == "markdown" || ext == "mdx";
        };
        if (!is_md(f.filename))
            co_return JsonResponse(pool, 400,
                "{\"message\":" + JsonStr("仅支持 .md / .markdown / .mdx 文件") + "}");

        // 文本按 UTF-8 base64 编码（图片上传同样的编码路径）
        const std::string data_url = "data:text/markdown;charset=utf-8;base64," +
                                     Base64Encode(f.data);

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();
        std::string sql = "INSERT INTO resources(name,url,type) VALUES('";
        sql += SqlEscape(raw, f.filename) + "','";
        sql += SqlEscape(raw, data_url) + "','md')";
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string resp_body = "{\"url\":" + JsonStr(data_url) +
                                ",\"name\":" + JsonStr(f.filename) + "}";
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// ═══════════════════════════════════════════════════════════════════
// 用户管理（后台「系统管理 → 用户管理」面板）
// ═══════════════════════════════════════════════════════════════════

// GET /api/users → 用户列表（不含 password）
class UsersHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(
                    "SELECT id,username,created_at FROM users ORDER BY id", 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string resp_body = BuildUsersJson(res);
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// POST /api/users（upsert：body 含 id → UPDATE username/password，否则 INSERT）
// password 留空表示不改密码（编辑场景），新增时必须填
class SaveUserHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string body(ctx.Body());
        const std::string id = JsonField(body, "id");
        const std::string username = JsonField(body, "username");
        const std::string password = JsonField(body, "password");
        if (username.empty())
            co_return JsonResponse(pool, 400, R"({"message":"用户名不能为空"})");

        // 用户 id 是 INT AUTO_INCREMENT：非法 id（如 Date.now() 时间戳）一律按新增
        auto is_valid_id = [](const std::string& s) {
            if (s.empty()) return false;
            long long v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
                if (v > 2147483647LL) return false;
            }
            return v >= 1;
        };
        const bool has_valid_id = is_valid_id(id);

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();
        auto esc = [raw](const std::string& s) { return SqlEscape(raw, s); };

        // username 唯一性校验：排除自身（编辑场景 id 相同不算冲突）
        std::string dup_sql = "SELECT id FROM users WHERE username='";
        dup_sql += esc(username) + "'";
        if (has_valid_id) dup_sql += " AND id<>'" + esc(id) + "'";
        dup_sql += " LIMIT 1";
        MYSQL_RES* dup = nullptr;
        try {
            dup = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(dup_sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        bool dup_found = (dup && mysql_num_rows(dup) > 0);
        if (dup) mysql_free_result(dup);
        if (dup_found)
            co_return JsonResponse(pool, 400,
                "{\"message\":" + JsonStr("用户名已存在") + "}");

        std::string sql;
        bool is_update = false;
        if (has_valid_id) {
            // 合法 id：先查是否存在，存在才 UPDATE（编辑），否则 INSERT 复用该 id
            std::string check = "SELECT id FROM users WHERE id='" + esc(id) + "' LIMIT 1";
            MYSQL_RES* chk = nullptr;
            try {
                chk = co_await coro::AwaitTask<MYSQL_RES*>{
                    g.get()->async_query(check.c_str(), 5000)};
            } catch (const std::exception& e) {
                if (chk) mysql_free_result(chk);
                co_return JsonResponse(pool, 500,
                    "{\"message\":" + JsonStr(e.what()) + "}");
            }
            bool exists = (chk && mysql_num_rows(chk) > 0);
            if (chk) mysql_free_result(chk);
            is_update = exists;
        }
        if (is_update) {
            sql = "UPDATE users SET username='" + esc(username) + "'";
            // 编辑时 password 留空表示不改密码
            if (!password.empty()) sql += ",password='" + esc(password) + "'";
            sql += " WHERE id='" + esc(id) + "'";
        } else {
            // 新增必须有密码，否则无法登录
            if (password.empty())
                co_return JsonResponse(pool, 400,
                    R"({"message":"新增用户必须填写密码"})");
            sql = "INSERT INTO users(id,username,password) VALUES(" +
                  (has_valid_id ? "'" + esc(id) + "'" : std::string("NULL")) +
                  ",'" + esc(username) + "','" + esc(password) + "')";
        }
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"保存成功"})");
    }
};

// DELETE /api/users/:id
// 保护：禁止删除 id=1 的种子管理员（删掉后无法再登录后台）
class DeleteUserHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        const std::string id(ctx.Param("id"));
        if (id.empty()) co_return JsonResponse(pool, 400, R"({"message":"缺少用户 id"})");
        if (id == "1")
            co_return JsonResponse(pool, 400,
                R"({"message":"不能删除种子管理员（admin）"})");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "DELETE FROM users WHERE id='";
        sql += SqlEscape(g.get()->raw(), id);
        sql += "'";
        try {
            (void)co_await coro::AwaitTask<uint64_t>{
                g.get()->async_update(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        co_return JsonResponse(pool, 200, R"({"message":"删除成功"})");
    }
};

// ── 访问统计：/api/stats 历史聚合（数据源 site_stats 分钟落库）──

// site_stats 分组聚合结果 → JSON（range/total/points）。行列：
// 0=bucket 1=req 2=err 3=bytes 4=avg(p50) 5=avg(p90) 6=avg(p99)
// 7=avg(act) 8=max(act_max) 9=max(p99)
std::string BuildStatsJson(MYSQL_RES* res, const std::string& range)
{
    std::string body = "{\"range\":" + JsonStr(range)
        + ",\"points\":[";
    if (!res) { body += "],\"total\":{\"req\":0,\"err\":0,\"bytes\":0,\"p99_max\":0}}"; return body; }

    uint64_t t_req = 0, t_err = 0, t_bytes = 0, t_p99max = 0;
    bool first = true;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        uint64_t req  = row[1] ? strtoull(row[1], nullptr, 10) : 0;
        uint64_t err  = row[2] ? strtoull(row[2], nullptr, 10) : 0;
        uint64_t bytes= row[3] ? strtoull(row[3], nullptr, 10) : 0;
        uint64_t p50  = row[4] ? strtoull(row[4], nullptr, 10) : 0;
        uint64_t p90  = row[5] ? strtoull(row[5], nullptr, 10) : 0;
        uint64_t p99  = row[6] ? strtoull(row[6], nullptr, 10) : 0;
        uint64_t act  = row[7] ? (uint64_t)strtod(row[7], nullptr) : 0;
        uint64_t p99max = row[9] ? strtoull(row[9], nullptr, 10) : 0;
        t_req += req; t_err += err; t_bytes += bytes;
        if (p99max > t_p99max) t_p99max = p99max;
        if (!first) body += ",";
        first = false;
        body += "{\"ts\":" + JsonStr(row[0] ? row[0] : "")
              + ",\"req\":" + std::to_string(req)
              + ",\"err\":" + std::to_string(err)
              + ",\"bytes\":" + std::to_string(bytes)
              + ",\"p50\":" + std::to_string(p50)
              + ",\"p90\":" + std::to_string(p90)
              + ",\"p99\":" + std::to_string(p99)
              + ",\"act\":" + std::to_string(act) + "}";
    }
    body += "]";
    body += ",\"total\":{\"req\":" + std::to_string(t_req)
          + ",\"err\":" + std::to_string(t_err)
          + ",\"bytes\":" + std::to_string(t_bytes)
          + ",\"p99_max\":" + std::to_string(t_p99max) + "}}";
    return body;
}

// GET /api/stats?range=24h|7d → site_stats 历史聚合（后台仪表盘趋势图数据源）
class StatsHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");

        std::string range = std::string(ctx.Query("range"));
        if (range.empty()) range = "24h";
        const bool is_7d = (range == "7d");
        // 桶粒度：24h → 小时，7d → 天；ORDER BY 按 bucket 字符串（=时间序）
        const char* fmt      = is_7d ? "%Y-%m-%d" : "%Y-%m-%d %H:00";
        const char* interval = is_7d ? "7 DAY"    : "1 DAY";

        std::string sql = std::string("SELECT DATE_FORMAT(ts,'") + fmt + "') AS bucket,"
            " SUM(req), SUM(err), SUM(bytes),"
            " AVG(p50), AVG(p90), AVG(p99), AVG(act), MAX(act_max), MAX(p99)"
            " FROM site_stats WHERE ts >= NOW() - INTERVAL " + interval +
            " GROUP BY bucket ORDER BY bucket";

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        std::string body = BuildStatsJson(res, range);
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, 200, body);
    }
};

} // namespace

// ── 访问统计落库：每 60s 聚合实时指标写入 site_stats（分钟粒度）──
coro::Task<void> PersistSiteStats(MetricsCollector* mc, const MysqlConfig& cfg)
{
    if (!mc) co_return;
    StatsWindow w;                       // 顶层结构体（metrics.hpp），非 MetricsCollector 成员
    if (!mc->SumLast60s(w)) co_return;   // 该分钟无访问，跳过

    ConnGuard g(GetPool(cfg));
    if (!co_await g.borrow()) {
        Logger::Log(LogLevel::Warn, "STATS", "落库失败：连接池借用失败");
        co_return;
    }

    char tsbuf[32];
    time_t t = static_cast<time_t>(w.ts);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    // ON DUPLICATE KEY UPDATE 幂等：同分钟重启后重写该行
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO site_stats (ts,req,req_h1,req_h2,err,bytes,p50,p90,p99,act,act_max) "
        "VALUES ('%s',%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu) "
        "ON DUPLICATE KEY UPDATE req=VALUES(req),req_h1=VALUES(req_h1),"
        "req_h2=VALUES(req_h2),err=VALUES(err),bytes=VALUES(bytes),"
        "p50=VALUES(p50),p90=VALUES(p90),p99=VALUES(p99),act=VALUES(act),act_max=VALUES(act_max)",
        tsbuf,
        (unsigned long long)w.req, (unsigned long long)w.req_h1,
        (unsigned long long)w.req_h2, (unsigned long long)w.err,
        (unsigned long long)w.bytes, (unsigned long long)w.per.p50,
        (unsigned long long)w.per.p90, (unsigned long long)w.per.p99,
        (unsigned long long)(w.act_avg + 0.5), (unsigned long long)w.act_max);
    try {
        // async_update 返回 Task<uint64_t>（受影响行数），与其它 handler 一致
        (void)co_await coro::AwaitTask<uint64_t>{g.get()->async_update(sql, 5000)};
    } catch (const std::exception& e) {
        // 失败不阻塞 FlushLoop：记日志，下个 60s 周期自动重试（可容忍丢一条）
        Logger::Log(LogLevel::Warn, "STATS",
                    std::string("落库失败: ") + e.what());
    }
}

// ═══════════════════════════════════════════════════════════════════
// 局域网访问开关：判断与接口
// ═══════════════════════════════════════════════════════════════════
namespace {

// 判断请求 Host 是否为「局域网 IP」访问。
// 关闭状态下只有局域网设备会用 `http://<IP>:<port>` 访问（Host 即该 IP），
// 本机后台走 localhost/127.0.0.1，据此区分来源（Docker NAT 后源 IP 不可用）。
// 返回 false（放行）的情况：空、IPv6 形式、localhost、回环、非点分主机名。
bool IsLanHost(std::string_view host) {
    if (host.empty()) return false;
    // IPv6（如 [::1]:8443）保守放行
    if (host.find('[') != std::string_view::npos ||
        host.find(']') != std::string_view::npos)
        return false;
    std::string h(host);
    auto colon = h.rfind(':');
    if (colon != std::string::npos) h = h.substr(0, colon);  // 去掉 :端口
    if (h == "localhost" || h == "127.0.0.1" || h == "0.0.0.0" || h == "::1")
        return false;
    // 点分 IPv4（三个点）视为局域网访问
    int dots = 0;
    for (char c : h)
        if (c == '.') ++dots;
    return dots == 3;
}

// 解析 JSON 布尔字段（前端 JSON.stringify 输出无引号 true/false，
// JsonField 只支持带引号字符串值，布尔需单独解析；兼容 "true"/"false"）
bool JsonBool(std::string_view body, std::string_view key, bool def = false) {
    const std::string needle = "\"" + std::string(key) + "\"";
    auto p = body.find(needle);
    if (p != std::string_view::npos) {
        auto colon = body.find(':', p + needle.size());
        if (colon != std::string_view::npos) {
            auto i = body.find_first_not_of(" \t", colon + 1);
            if (i != std::string_view::npos) {
                if (body.substr(i, 4) == "true") return true;
                if (body.substr(i, 5) == "false") return false;
            }
        }
    }
    // 兼容带引号形式 "true"/"false"
    return JsonField(body, key) == "true";
}

// GET/POST /api/network/lan
//   GET  → {enabled, lanIp}（前端工作区开关初始化）
//   POST → 更新内存 + 落库 site_config，返回最新状态
class LanStatusHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");
        if (ctx.Method() == "POST") {
            const std::string body(ctx.Body());
            const bool enabled = JsonBool(body, "enabled");
            g_lan_enabled.store(enabled);

            // 落库（ON DUPLICATE KEY UPDATE 幂等）；失败不阻塞本次开关生效
            ConnGuard g(GetPool(cfg_));
            if (co_await g.borrow()) {
                const std::string sql =
                    "INSERT INTO site_config(`key`,`value`) VALUES('lan_enabled','"
                    + std::string(enabled ? "1" : "0")
                    + "') ON DUPLICATE KEY UPDATE `value`=VALUES(`value`)";
                try {
                    (void)co_await coro::AwaitTask<uint64_t>{
                        g.get()->async_update(sql.c_str(), 5000)};
                } catch (...) {
                    // 忽略：内存已生效，重启时以库为准（容忍丢一次落库）
                }
            }
        }
        std::string resp_body = "{\"enabled\":" +
            std::string(g_lan_enabled.load() ? "true" : "false") +
            ",\"lanIp\":" + JsonStr(g_lan_ip) + "}";
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// ── 站点全局配置（blog_config 表，后台「站点设置」读写） ──
// 存整个配置 JSON 字符串 {siteName,slogan,copyright,background,navMenus,workItems}，
// 前端首屏先渲染本地缓存、再拉后端覆盖，实现「所有设备同一份配置」。
// 后端只做字符串透传（不解析字段）：POST 原样落库，GET 原样返回。

// GET/POST /api/site-config
//   GET  → {"config": <JSON> | null}（无记录返回 null，前端用本地/默认值兜底）
//   POST → body 即整个配置 JSON 字符串，upsert 到 blog_config(key='blog')
class SiteConfigHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        MYSQL* raw = g.get()->raw();

        if (ctx.Method() == "POST") {
            const std::string body(ctx.Body());
            // 1MB 上限：MEDIUMTEXT 上限 16MB，配置含背景图片路径一般远小于此
            if (body.empty() || body.size() > 1048576)
                co_return JsonResponse(pool, 400, R"({"message":"配置内容无效"})");
            const std::string sql =
                "INSERT INTO blog_config(`key`,`value`) VALUES('blog','"
                + SqlEscape(raw, body)
                + "') ON DUPLICATE KEY UPDATE `value`=VALUES(`value`)";
            try {
                (void)co_await coro::AwaitTask<uint64_t>{
                    g.get()->async_update(sql.c_str(), 5000)};
            } catch (const std::exception& e) {
                co_return JsonResponse(pool, 500,
                    "{\"message\":" + JsonStr(e.what()) + "}");
            }
            co_return JsonResponse(pool, 200, R"({"ok":true})");
        }

        // GET：读 key='blog' 的配置，原样拼进 {"config": <JSON>}
        const std::string sql = "SELECT `value` FROM blog_config WHERE `key`='blog'";
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }
        MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
        const char* val = (row && row[0]) ? row[0] : nullptr;
        std::string resp_body = val
            ? "{\"config\":" + std::string(val) + "}"
            : R"({"config":null})";
        if (res) mysql_free_result(res);
        co_return JsonResponse(pool, 200, resp_body);
    }
};

// ── 访问者统计 ──

// 上报请求是否来自本机（转发器连 127.0.0.1:9443 上报，Host=127.0.0.1:9443；
// 外部设备经转发器的 Host 是局域网 IP，拒绝，防止伪造灌库）
bool IsLocalHost(std::string_view host) {
    if (host.empty()) return false;
    return host.find("127.0.0.1") != std::string_view::npos ||
           host.find("localhost") != std::string_view::npos;
}

// 当前在线 IP 数 / 指定 IP 是否在线
int ActiveVisitorCount() {
    std::lock_guard<std::mutex> lk(g_visitor_mutex);
    return static_cast<int>(g_active_visitors.size());
}
bool IsOnline(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_visitor_mutex);
    auto it = g_active_visitors.find(ip);
    return it != g_active_visitors.end() && it->second > 0;
}

// GET/POST /api/network/visitor
//   POST {ip, action:"connect"|"disconnect"}（仅本机转发器上报）
//     connect    → 在线 +1，UPSERT visitor_stats（cnt+1, last_seen=NOW）
//     disconnect → 在线 -1
//   GET ?limit=20 → {onlineCount, visitors:[{ip,cnt,firstSeen,lastSeen,online}]}
class VisitorHandler : public MysqlHandlerBase {
public:
    using MysqlHandlerBase::MysqlHandlerBase;
    coro::Task<Response> HandleAsync(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) co_return Response::Raw(500, "no pool");

        if (ctx.Method() == "POST") {
            if (!IsLocalHost(ctx.Header("host")))
                co_return JsonResponse(pool, 403, R"({"message":"拒绝上报"})");
            const std::string body(ctx.Body());
            const std::string ip = JsonField(body, "ip");
            const std::string action = JsonField(body, "action");
            if (ip.empty())
                co_return JsonResponse(pool, 400, R"({"message":"缺少 ip"})");

            if (action == "connect") {
                {
                    std::lock_guard<std::mutex> lk(g_visitor_mutex);
                    g_active_visitors[ip] += 1;
                }
                // 落库累计访问（UPSERT 幂等：cnt+1，last_seen 刷新；首访记 first_seen）
                ConnGuard g(GetPool(cfg_));
                if (co_await g.borrow()) {
                    std::string sql =
                        "INSERT INTO visitor_stats(ip,cnt,first_seen,last_seen) "
                        "VALUES('" + SqlEscape(g.get()->raw(), ip) + "',1,NOW(),NOW()) "
                        "ON DUPLICATE KEY UPDATE cnt=cnt+1, last_seen=NOW()";
                    try {
                        (void)co_await coro::AwaitTask<uint64_t>{
                            g.get()->async_update(sql.c_str(), 5000)};
                    } catch (...) {
                        // 落库失败不影响在线状态
                    }
                }
            } else if (action == "disconnect") {
                std::lock_guard<std::mutex> lk(g_visitor_mutex);
                auto it = g_active_visitors.find(ip);
                if (it != g_active_visitors.end()) {
                    if (--(it->second) <= 0) g_active_visitors.erase(it);
                }
            }
            co_return JsonResponse(pool, 200, R"({"ok":true})");
        }

        // GET：最近访问者（按最近访问倒序）+ 在线标记
        const std::string limit_str = std::string(ctx.Query("limit"));
        int limit = 20;
        if (!limit_str.empty()) limit = std::atoi(limit_str.c_str());
        if (limit <= 0) limit = 20;
        if (limit > 200) limit = 200;

        ConnGuard g(GetPool(cfg_));
        if (!co_await g.borrow())
            co_return JsonResponse(pool, 500, R"({"message":"数据库连接失败"})");
        std::string sql = "SELECT ip,cnt,first_seen,last_seen FROM visitor_stats "
                          "ORDER BY last_seen DESC LIMIT " + std::to_string(limit);
        MYSQL_RES* res = nullptr;
        try {
            res = co_await coro::AwaitTask<MYSQL_RES*>{
                g.get()->async_query(sql.c_str(), 5000)};
        } catch (const std::exception& e) {
            co_return JsonResponse(pool, 500,
                "{\"message\":" + JsonStr(e.what()) + "}");
        }

        std::string body = "{\"onlineCount\":" +
            std::to_string(ActiveVisitorCount()) + ",\"visitors\":[";
        bool first = true;
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                if (!first) body += ",";
                first = false;
                const std::string ip = row[0] ? row[0] : "";
                body += "{\"ip\":" + JsonStr(ip) +
                        ",\"cnt\":" + (row[1] ? row[1] : "0") +
                        ",\"firstSeen\":" + JsonStr(row[2] ? row[2] : "") +
                        ",\"lastSeen\":" + JsonStr(row[3] ? row[3] : "") +
                        ",\"online\":" + (IsOnline(ip) ? "true" : "false") + "}";
            }
            mysql_free_result(res);
        }
        body += "]}";
        co_return JsonResponse(pool, 200, body);
    }
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════
// 局域网访问开关：中间件与启动初始化
// ═══════════════════════════════════════════════════════════════════

// 关闭时拦截局域网 IP Host 请求（403），localhost/127.0.0.1 始终放行
Response LanGuardMiddleware::HandlePre(Context& ctx) {
    if (g_lan_enabled.load()) return Response::None();
    if (!IsLanHost(ctx.Header("host"))) return Response::None();

    static const std::string kBody =
        "{\"message\":\"局域网访问已关闭\"}";
    std::string raw = "HTTP/1.1 403 Forbidden\r\n"
                      "Content-Type: application/json\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " + std::to_string(kBody.size()) + "\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n" + kBody;
    return Response::Raw(403, raw);
}

bool IsLanEnabled() { return g_lan_enabled.load(); }
std::string LanIp() { return g_lan_ip; }

// 启动时（main 线程，同步 MySQL）读 site_config 初始化开关状态；
// 表/行不存在时保持默认（关闭）。g_lan_ip 来自环境变量 HOST_LAN_IP。
void InitLanStateFromDb(const MysqlConfig& cfg) {
    const char* env = std::getenv("HOST_LAN_IP");
    g_lan_ip = env ? env : "";

    MYSQL* conn = mysql_init(nullptr);
    if (!conn) return;
    if (!mysql_real_connect(conn, cfg.host.c_str(), cfg.user.c_str(),
                            cfg.password.c_str(), cfg.database.c_str(),
                            cfg.port, nullptr, 0)) {
        mysql_close(conn);
        return;
    }
    // 建表幂等（init.sql 不可重跑，site_config / visitor_stats 靠这里创建）
    mysql_query(conn,
        "CREATE TABLE IF NOT EXISTS site_config ("
        "`key` VARCHAR(64) PRIMARY KEY,"
        "`value` VARCHAR(255) NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    // 站点全局配置（blogConfig + workItems 的 JSON 字符串）：
    // value 用 MEDIUMTEXT（site_config 的 VARCHAR(255) 存不下完整配置）。
    // 后台「站点设置」保存 → POST /api/site-config 落这里，所有设备读同一份。
    mysql_query(conn,
        "CREATE TABLE IF NOT EXISTS blog_config ("
        "`key` VARCHAR(64) PRIMARY KEY,"
        "`value` MEDIUMTEXT"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    // 访问者统计：ip 唯一，cnt 累计连接次数，first/last_seen 首次/最近访问
    mysql_query(conn,
        "CREATE TABLE IF NOT EXISTS visitor_stats ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "ip VARCHAR(45) NOT NULL,"
        "cnt INT NOT NULL DEFAULT 0,"
        "first_seen DATETIME,"
        "last_seen DATETIME,"
        "UNIQUE KEY uk_ip (ip)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    if (mysql_query(conn, "SELECT `value` FROM site_config WHERE `key`='lan_enabled'") == 0) {
        MYSQL_RES* res = mysql_store_result(conn);
        if (res) {
            if (mysql_num_rows(res) > 0) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row && row[0])
                    g_lan_enabled.store(std::string(row[0]) == "1");
            }
            mysql_free_result(res);
        }
    }
    mysql_close(conn);
}

// ═══════════════════════════════════════════════════════════════════
// 路由注册
// ═══════════════════════════════════════════════════════════════════
void RegisterBlogRoutes(Router& router, const MysqlConfig& cfg) {
    router.Get   ("/api/network/lan",   std::make_unique<LanStatusHandler>(cfg));
    router.Post  ("/api/network/lan",   std::make_unique<LanStatusHandler>(cfg));
    router.Get   ("/api/network/visitor", std::make_unique<VisitorHandler>(cfg));
    router.Post  ("/api/network/visitor", std::make_unique<VisitorHandler>(cfg));
    router.Get   ("/api/site-config",   std::make_unique<SiteConfigHandler>(cfg));
    router.Post  ("/api/site-config",   std::make_unique<SiteConfigHandler>(cfg));
    router.Post  ("/api/login",         std::make_unique<LoginHandler>(cfg));
    router.Get   ("/api/stats",         std::make_unique<StatsHandler>(cfg));
    router.Get   ("/api/articles",      std::make_unique<ArticlesHandler>(cfg));
    router.Get   ("/api/articles/:id",  std::make_unique<ArticleByIdHandler>(cfg));
    router.Post  ("/api/articles",      std::make_unique<SaveArticleHandler>(cfg));
    router.Delete("/api/articles/:id",  std::make_unique<DeleteArticleHandler>(cfg));
    router.Get   ("/api/categories",    std::make_unique<CategoriesHandler>(cfg));
    router.Post  ("/api/categories",    std::make_unique<SaveCategoryHandler>(cfg));
    router.Delete("/api/categories/:id",std::make_unique<DeleteCategoryHandler>(cfg));
    router.Get   ("/api/resources",             std::make_unique<ResourcesHandler>(cfg));
    router.Get   ("/api/resources/:id",         std::make_unique<ResourceByIdHandler>(cfg));
    // 文件接口用独立前缀 /api/img/:id：router 在同一个父节点下 param(:id) 与静态子节点
    // 混合时，param 消费段后 path 保留前导 '/'，静态子节点匹配有缺陷（会 404）。
    // 独立前缀避开 param/静态混用，与 /api/users/:id 一样是「静态 + 末尾 param」的常规形态。
    router.Get   ("/api/img/:id",               std::make_unique<ResourceImageHandler>(cfg));
    router.Delete("/api/resources/:id",         std::make_unique<DeleteResourceHandler>(cfg));
    router.Post  ("/api/upload/image",  std::make_unique<UploadImageHandler>(cfg));
    router.Post  ("/api/upload/md",     std::make_unique<UploadMdHandler>(cfg));
    router.Get   ("/api/users",         std::make_unique<UsersHandler>(cfg));
    router.Post  ("/api/users",         std::make_unique<SaveUserHandler>(cfg));
    router.Delete("/api/users/:id",     std::make_unique<DeleteUserHandler>(cfg));
}
