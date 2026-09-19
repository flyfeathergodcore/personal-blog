// ═══════════════════════════════════════════════════════════════════
// router_test — 路由层单元测试
//
// 覆盖：query string 剥离匹配、:param / * 参数捕获（不含 query）、
// lambda 函数式注册、类式注册（unique_ptr 重载不被模板劫持）、
// Context 的 Param/Query 端到端（真实 H1Parser + SessionRegion）。
// ═══════════════════════════════════════════════════════════════════
#include "http/router/router.hpp"
#include "http/protocol/context.hpp"
#include "http/protocol/session_region.hpp"
#include "http/protocol/region_pool.hpp"
#include "http/protocol/http1.1/parser.hpp"
#include "http/handler/request_handler.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (cond) { ++g_pass; } else {                                          \
            ++g_fail; std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
    } while (0)

// 最小的可调用 handler（返回 None，用于验证路由命中/参数捕获）
class TestHandler : public RequestHandler {
public:
    explicit TestHandler(const char* name) : name_(name) {}
    Response Handle(const Context&) override { return Response::None(); }
    const char* name_;
};

// 最小 Context 桩：仅提供固定 Path，Pool()==nullptr。
// 用于直接调用 lambda handler（Response::Raw 不依赖 pool）。
class CtxStub : public Context {
public:
    explicit CtxStub(std::string_view path) : path_(path) {}
    ParseResult Feed(const char*, size_t) override { return ParseResult::Complete; }
    std::string_view Method()  const override { return "GET"; }
    std::string_view Path()    const override { return path_; }
    std::string_view Version() const override { return "HTTP/1.1"; }
    std::string_view Header(std::string_view) const override { return {}; }
    std::string_view Body()    const override { return {}; }
    int HeaderCount() const override { return 0; }
    std::pair<std::string_view, std::string_view> HeaderAt(int) const override {
        return {};
    }
private:
    std::string_view path_;
};

// ── 1. Match 剥 query：带 query 命中注册的精确路由（原 404 bug）──
static void test_query_stripping()
{
    Router r;
    r.Get("/hello", std::make_unique<TestHandler>("hello"));
    auto* h = r.Match("GET", "/hello?name=alice");
    CHECK(h != nullptr, "GET /hello?name=alice 命中 /hello");
}

// ── 2. 单参 Match（path-only, any-method）也剥 query ──
static void test_path_only_match_strips_query()
{
    Router r;
    r.Get("/hello", std::make_unique<TestHandler>("hello"));
    auto* h = r.Match("/hello?x=1");
    CHECK(h != nullptr, "单参 Match 剥 query 命中 /hello");
}

// ── 3. :param 捕获，query 不被吞进参数值 ──
static void test_param_capture_excludes_query()
{
    Router r;
    r.Get("/users/:id", std::make_unique<TestHandler>("user"));
    std::vector<std::pair<std::string_view, std::string_view>> params;
    auto* h = r.Match("GET", "/users/42?tab=info", &params);
    CHECK(h != nullptr, "GET /users/42?tab=info 命中 /users/:id");
    CHECK(params.size() == 1, ":param 捕获 1 个参数");
    CHECK(params[0].first == "id" && params[0].second == "42",
          ":param id 捕获为 42（不含 query）");
}

// ── 4. * catch-all 捕获，query 不吞进参数值 ──
static void test_catchall_excludes_query()
{
    Router r;
    r.Add("/files/*", std::make_unique<TestHandler>("files"));
    std::vector<std::pair<std::string_view, std::string_view>> params;
    auto* h = r.Match("GET", "/files/a/b.txt?v=2", &params);
    CHECK(h != nullptr, "GET /files/a/b.txt?v=2 命中 /files/*");
    CHECK(params.size() == 1, "catch-all 捕获 1 个参数");
    CHECK(params[0].first == "path" && params[0].second == "a/b.txt",
          "catch-all path 捕获为 a/b.txt（不含 query）");
}

// ── 5. lambda 函数式注册 + 调用 ──
static void test_lambda_register()
{
    Router r;
    bool called = false;
    r.Get("/lambda", [&](const Context&) -> Response {
        called = true;
        return Response::Raw(200, "ok");
    });
    auto* h = r.Match("GET", "/lambda?p=1");
    CHECK(h != nullptr, "lambda 路由注册且带 query 命中");
    CHECK(h != nullptr && h->IsStream() == false, "lambda 是同步 handler");

    // 直接调用（CtxStub 提供 Path，Response::Raw 不需 pool）
    CtxStub stub("/lambda");
    auto resp = h->Handle(stub);
    CHECK(called, "lambda handler 被调用");
    CHECK(resp.StatusCode() == 200, "lambda 返回 200");
}

// ── 6. 类式注册仍绑 unique_ptr 重载（防模板劫持回归）──
static void test_class_register_not_hijacked()
{
    Router r;
    r.Get("/c", std::make_unique<TestHandler>("c"));   // 必须编译且命中
    auto* h = r.Match("GET", "/c");
    CHECK(h != nullptr, "类式 unique_ptr 注册仍工作（未被模板劫持）");
}

// ── 7. Context Param/Query 端到端：真实 H1Parser + SessionRegion ──
static void test_context_params_query()
{
    RegionPool pool;
    SessionRegion region;
    region.Init(&pool);

    H1Parser p;
    p.SetPool(&region);

    const char* req = "GET /users/42?name=alice HTTP/1.1\r\n"
                      "Host: x\r\n\r\n";
    auto r = p.Feed(req, std::strlen(req));
    CHECK(r == ParseResult::Complete, "H1Parser Feed 完整请求");

    // Path() 保持完整 URI（含 query，供转发）
    CHECK(p.Path() == "/users/42?name=alice", "ctx.Path() 保持完整 URI");

    // ctx.Query 惰性解析
    CHECK(p.Query("name") == "alice", "ctx.Query(\"name\") == alice");
    CHECK(p.Query("missing") == "", "ctx.Query 不存在的 key 返回空");
    CHECK(p.QueryCount() == 1, "QueryCount == 1");

    // Router 捕获 params → SetParams → ctx.Param
    Router router;
    router.Get("/users/:id", std::make_unique<TestHandler>("user"));
    std::vector<std::pair<std::string_view, std::string_view>> params;
    auto* h = router.Match("GET", p.Path(), &params);
    CHECK(h != nullptr, "真实 Path（含 query）命中 /users/:id");
    p.SetParams(params);

    CHECK(p.Param("id") == "42", "ctx.Param(\"id\") == 42");
    CHECK(p.ParamCount() == 1, "ParamCount == 1");
    CHECK(p.ParamAt(0).first == "id" && p.ParamAt(0).second == "42",
          "ParamAt(0) == (id, 42)");
}

// ── 8. keep-alive 下 params/query 每请求重置 ──
static void test_request_state_reset()
{
    RegionPool pool;
    SessionRegion region;
    region.Init(&pool);
    H1Parser p;
    p.SetPool(&region);

    // 请求 1：带 query 和 param
    const char* req1 = "GET /users/42?name=alice HTTP/1.1\r\n\r\n";
    CHECK(p.Feed(req1, std::strlen(req1)) == ParseResult::Complete, "req1 parse");
    Router r;
    r.Get("/users/:id", std::make_unique<TestHandler>("user"));
    std::vector<std::pair<std::string_view, std::string_view>> params;
    r.Match("GET", p.Path(), &params);
    p.SetParams(params);
    CHECK(p.Param("id") == "42", "req1 param set");

    // 请求 2：无 query 无 param，Feed 开头应清掉请求 1 的残留
    const char* req2 = "GET /plain HTTP/1.1\r\n\r\n";
    CHECK(p.Feed(req2, std::strlen(req2)) == ParseResult::Complete, "req2 parse");
    CHECK(p.ParamCount() == 0, "req2 param 已清空");
    CHECK(p.Param("id") == "", "req2 Param(id) 为空");
    CHECK(p.QueryCount() == 0, "req2 query 已清空");
    CHECK(p.Query("name") == "", "req2 Query(name) 为空");
}

int main()
{
    test_query_stripping();
    test_path_only_match_strips_query();
    test_param_capture_excludes_query();
    test_catchall_excludes_query();
    test_lambda_register();
    test_class_register_not_hijacked();
    test_context_params_query();
    test_request_state_reset();

    std::printf("\nrouter_test: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
