#include "http/router/router.hpp"
#include "http/handler/request_handler.hpp"
#include "http/handler/health.hpp"
#include "http/handler/reverse_proxy.hpp"
#include "http/cache/file_cache.hpp"
#include <cstring>
#include <iostream>

// ═══════════════════════════════════════════════════════════════════
// Node helpers
// ═══════════════════════════════════════════════════════════════════

// 设置指定 HTTP 方法的 handler；非标准方法存入 extra 表
// 参数：method - HTTP 方法；h - 处理器指针
void Router::Node::SetHandler(std::string_view method, RequestHandler* h)
{
    if (method == "GET")              handler_get    = h;
    else if (method == "POST")        handler_post   = h;
    else if (method == "PUT")         handler_put    = h;
    else if (method == "DELETE")      handler_delete = h;
    else if (method == "HEAD")        handler_head   = h;
    else {
        if (!extra)
            extra = std::make_unique<std::unordered_map<std::string, RequestHandler*>>();
        (*extra)[std::string(method)] = h;
    }
}

// 获取指定 HTTP 方法的 handler（HEAD 回退到 GET）；无则返回 nullptr
// 参数：method - HTTP 方法
RequestHandler* Router::Node::GetHandler(std::string_view method) const
{
    if (method == "GET")              return handler_get;
    if (method == "POST")             return handler_post;
    if (method == "PUT")              return handler_put;
    if (method == "DELETE")           return handler_delete;
    if (method == "HEAD") {
        // HEAD → GET fallback
        return handler_head ? handler_head : handler_get;
    }
    if (extra) {
        auto it = extra->find(std::string(method));
        if (it != extra->end()) return it->second;
    }
    return nullptr;
}

// 判断本节点是否持有任意方法的 handler
bool Router::Node::HasHandler() const
{
    return handler_any || handler_get || handler_post
        || handler_put || handler_delete || handler_head
        || (extra && !extra->empty());
}

// 添加静态子节点并记录其首字节到 indices
// 参数：child - 子节点；childFirstByte - 子节点 path 的首字节；返回子节点裸指针
Router::Node* Router::Node::AddStaticChild(std::unique_ptr<Node> child, char childFirstByte)
{
    auto* raw = child.get();
    indices += childFirstByte;
    children.push_back(std::move(child));
    return raw;
}

// 添加 :param 动态子节点并设置 paramChild 快指针
// 参数：child - 参数子节点；返回子节点裸指针
Router::Node* Router::Node::AddParamChild(std::unique_ptr<Node> child)
{
    auto* raw = child.get();
    paramChild = raw;
    children.push_back(std::move(child));
    return raw;
}

// 添加 *catchAll 兜底子节点并设置 catchAllChild 快指针
// 参数：child - 兜底子节点；返回子节点裸指针
Router::Node* Router::Node::AddCatchAllChild(std::unique_ptr<Node> child)
{
    auto* raw = child.get();
    catchAllChild = raw;
    children.push_back(std::move(child));
    return raw;
}

// ═══════════════════════════════════════════════════════════════════
// Router 构造 / 析构
// ═══════════════════════════════════════════════════════════════════

// 构造 Router：创建根节点
Router::Router()
    : root_(std::make_unique<Node>()) {}

// 析构：释放所有 handler 与节点
Router::~Router() = default;

// ═══════════════════════════════════════════════════════════════════
// SetupFromConfig — auto-register routes from config
// ═══════════════════════════════════════════════════════════════════

// 从配置自动注册路由：静态文件兜底、代理路由、重定向规则、健康检查
// 参数：cfg - 服务配置
void Router::SetupFromConfig(const Config& cfg)
{
    // Static file handler (default route)
    if (!file_cache_)
        file_cache_ = std::make_unique<FileCache>();
    file_cache_->LoadDirectory(cfg.doc_root);
    Add("/", std::unique_ptr<StaticFileHandler>(
        new StaticFileHandler(file_cache_.get())));

    // Proxy routes（ReverseProxy 已移植回构建，Task 9）
    for (auto& pr : cfg.proxy_routes) {
        std::vector<UpstreamAddr> addrs = pr.upstreams;
        std::cout << "[route] " << pr.prefix << " → ReverseProxy" << std::endl;
        Add(pr.prefix,
            std::unique_ptr<ReverseProxy>(
                new ReverseProxy(std::move(addrs))));
    }

    // Redirect rules (registered before static files so they take priority)
    for (auto& rr : cfg.redirect_rules) {
        std::cout << "[route] " << rr.from << " → "
                  << rr.to << " (" << rr.code << ")" << std::endl;
        Add(rr.from,
            std::unique_ptr<RedirectHandler>(
                new RedirectHandler(rr.to, rr.code)));
        // Also match the trailing-slash variant (e.g., /old-path/ → same redirect)
        if (rr.from.size() > 1 && rr.from.back() != '/') {
            Add(rr.from + "/",
                std::unique_ptr<RedirectHandler>(
                    new RedirectHandler(rr.to, rr.code)));
        }
    }

    // Health check endpoint — registered before the catch-all static route
    std::cout << "[route] /healthz → HealthHandler" << std::endl;
    Add("/healthz",
        std::unique_ptr<HealthHandler>(new HealthHandler()));
}

// ═══════════════════════════════════════════════════════════════════
// 公开路由注册接口
// ═══════════════════════════════════════════════════════════════════

// 注册任意 HTTP 方法的 handler；尾部 "/" 视为前缀匹配
// 参数：path - 路由路径；handler - 处理器（独占所有权）
void Router::Add(std::string path, std::unique_ptr<RequestHandler> handler)
{
    std::unique_lock lock(rw_mutex_);
    bool prefix = false;
    // 尾部 "/" 表示前缀匹配，例如 "/api/" → "/api" + prefixMatch
    if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
        prefix = true;
    } else if (path == "/") {
        // "/" 始终作为前缀匹配 —— 兜底所有子路径
        prefix = true;
        path = "";
    }

    auto* raw = handler.release();
    handlers_.emplace_back(raw);

    auto* node = Insert(root_.get(), path, raw, "");
    if (prefix)
        node->isPrefixMatch = true;
    node->handler_any = raw;
}

// 注册指定 HTTP 方法的 handler
// 参数：method - HTTP 方法；path - 路由路径；handler - 处理器（独占所有权）
void Router::AddRoute(std::string method, std::string path,
                       std::unique_ptr<RequestHandler> handler)
{
    std::unique_lock lock(rw_mutex_);
    auto* raw = handler.release();
    handlers_.emplace_back(raw);

    auto* node = Insert(root_.get(), path, raw, method);
    node->SetHandler(method, raw);
}

// ═══════════════════════════════════════════════════════════════════
// 最长公共前缀
// ═══════════════════════════════════════════════════════════════════

// 计算两个路径的最长公共前缀长度
// 参数：a - 路径 a；b - 路径 b；返回公共前缀长度
size_t Router::LongestCommonPrefix(std::string_view a, std::string_view b)
{
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i])
        ++i;
    return i;
}

// ═══════════════════════════════════════════════════════════════════
// 查找通配符
// 返回第一个 :param 或 *catchAll（必须位于段首：i==0 || path[i-1]=='/'）
// 找不到时返回空 string_view
// ═══════════════════════════════════════════════════════════════════

// 查找路径中第一个 :param 或 *catchAll 通配符（须位于段首）
// 参数：path - 待查找路径；返回通配符子串，找不到返回空视图
std::string_view Router::FindWildcard(std::string_view path)
{
    for (size_t i = 0; i < path.size(); ++i) {
        if ((path[i] == ':' || path[i] == '*') &&
            (i == 0 || path[i - 1] == '/'))
        {
            auto end = path.find('/', i);
            if (end == std::string_view::npos)
                return path.substr(i);
            return path.substr(i, end - i);
        }
    }
    return {};
}

// ═══════════════════════════════════════════════════════════════════
// InsertChild — 在无共享前缀的节点下创建完整子树
//
// 有两种情况走到这里：
//   1. 全新的路由（树中不存在任何共享前缀）
//   2. 已有共享前缀，分裂后剩余的 path 需要建新子树
//
// 处理 :param / *catchAll 通配符，递归创建中间节点。
// ═══════════════════════════════════════════════════════════════════

// 在无共享前缀的节点下为剩余 path 创建完整子树（处理 :param / * 通配符）
// 参数：node - 父节点；path - 剩余路径；handler - 处理器；method - HTTP 方法（空表示任意方法）
void Router::InsertChild(Node* node, std::string_view path,
                          RequestHandler* handler, std::string_view method)
{
    while (true) {
        auto wildcard = FindWildcard(path);
        if (wildcard.empty()) {
            node->path.assign(path.data(), path.size());
            if (method.empty())
                node->handler_any = handler;
            else
                node->SetHandler(method, handler);
            return;
        }

        auto wildcardPos = static_cast<size_t>(wildcard.data() - path.data());
        if (wildcardPos > 0) {
            node->path.assign(path.data(), wildcardPos);
            path = path.substr(wildcardPos);
        }

        if (wildcard[0] == ':') {
            auto child = std::make_unique<Node>();
            child->nodeType  = Node::PARAM;
            child->path      = wildcard;
            child->paramName = std::string(wildcard.substr(1));
            if (child->paramName.empty()) child->paramName = "param";  // 无名 : 兜底

            node = node->AddParamChild(std::move(child));
            path = path.substr(wildcard.size());

            if (path.empty()) {
                if (method.empty())
                    node->handler_any = handler;
                else
                    node->SetHandler(method, handler);
                return;
            }

            auto rest = std::make_unique<Node>();
            node       = node->AddStaticChild(std::move(rest), path[0]);

        } else if (wildcard[0] == '*') {
            auto child = std::make_unique<Node>();
            child->nodeType  = Node::CATCH_ALL;
            child->path      = wildcard;
            child->paramName = std::string(wildcard.substr(1));
            if (child->paramName.empty()) child->paramName = "path";   // 无名 * 兜底


            if (method.empty())
                child->handler_any = handler;
            else
                child->SetHandler(method, handler);

            node->AddCatchAllChild(std::move(child));
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Insert — 向压缩前缀树插入一条路由
//
// 算法（gin/httprouter）：
//   1. 计算当前节点 path 与目标 path 的 LCP
//   2. 若 LCP < node.path → 分裂当前节点
//   3. 若 path 已被完全消费 → 设 handler 并返回
//   4. 若 path 尚有剩余 → 通过 indices 查找/创建子节点，递归
// ═══════════════════════════════════════════════════════════════════

// 向压缩前缀树插入路由：LCP 分裂 + 递归落位 handler
// 参数：node - 起始节点；path - 路由路径；handler - 处理器；method - HTTP 方法（空表示任意方法）；返回落位的节点
Router::Node* Router::Insert(Node* node, std::string_view path,
                              RequestHandler* handler, std::string_view method)
{
    if (node->path.empty() && node->children.empty() && !node->HasHandler()) {
        InsertChild(node, path, handler, method);
        return node;
    }

    while (true) {
        auto i = LongestCommonPrefix(node->path, path);

        if (i < node->path.size()) {
            auto split = std::make_unique<Node>();
            split->path     = node->path.substr(i);
            split->indices  = std::move(node->indices);
            split->children = std::move(node->children);

            split->paramChild    = node->paramChild;
            split->catchAllChild = node->catchAllChild;

            split->handler_any    = node->handler_any;
            split->handler_get    = node->handler_get;
            split->handler_post   = node->handler_post;
            split->handler_put    = node->handler_put;
            split->handler_delete = node->handler_delete;
            split->handler_head   = node->handler_head;
            split->extra          = std::move(node->extra);
            split->isPrefixMatch  = node->isPrefixMatch;

            node->path     = node->path.substr(0, i);
            node->indices  = split->path[0];
            node->children.clear();
            node->children.push_back(std::move(split));

            node->handler_any    = nullptr;
            node->handler_get    = nullptr;
            node->handler_post   = nullptr;
            node->handler_put    = nullptr;
            node->handler_delete = nullptr;
            node->handler_head   = nullptr;
            node->extra.reset();
            node->paramChild     = nullptr;
            node->catchAllChild  = nullptr;
            node->isPrefixMatch  = false;
        }

        if (i == path.size()) {
            if (!method.empty())
                node->SetHandler(method, handler);
            else
                node->handler_any = handler;
            return node;
        }

        path = path.substr(i);

        char c = path[0];
        bool matched = false;
        for (size_t j = 0; j < node->indices.size(); ++j) {
            if (c == node->indices[j]) {
                node    = node->children[j].get();
                matched = true;
                break;
            }
        }
        if (matched)
            continue;

        // 复用已注册的 :param 子节点：剩余路径恰好是 ":paramName"（前导 '/' 已被
        // 静态前缀节点消费）且 paramChild 已存在时，直接落位设置 handler。否则
        // （如先注册 GET /api/articles/:id 再注册 DELETE 同路径）会在 paramChild
        // 之外另建字面量子节点，method handler 落到错误节点 → 该 method 404。
        if (node->paramChild && path.size() >= 1 && path[0] == ':' &&
            node->paramChild->paramName == std::string(path.substr(1))) {
            node = node->paramChild;
            if (method.empty()) node->handler_any = handler;
            else node->SetHandler(method, handler);
            return node;
        }

        auto child = std::make_unique<Node>();
        auto* raw  = child.get();
        node->AddStaticChild(std::move(child), path[0]);
        InsertChild(raw, path, handler, method);
        return raw;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Lookup — 在压缩前缀树中查找路径
//
// 匹配优先级（每层）：
//   1. 精确静态匹配（indices 首字节 + path 全量比对）
//   2. :param 通配（捕获到下一个 '/'）
//   3. *catchAll 兜底（捕获剩余所有路径）
//   4. 若当前节点标记为 isPrefixMatch，作为回退
//   5. 参数 prefixFallback 携带上级前缀匹配节点
// ═══════════════════════════════════════════════════════════════════

// 在压缩前缀树中查找路径对应的节点，沿途收集路径参数
// 参数：node - 起始节点；path - 请求路径；params - 可选，接收捕获的参数；prefixFallback - 上级前缀匹配节点兜底；返回命中的节点或 nullptr
const Router::Node* Router::Lookup(
    const Node* node,
    std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* params,
    const Node* prefixFallback) const
{
    if (path.empty() || path == "/") {
        if (node->HasHandler() ||
            node->handler_any)
            return node;
        if (node->isPrefixMatch)
            return node;
        return nullptr;
    }

    if (path.size() > 1 && path.back() == '/')
        path.remove_suffix(1);

    while (true) {
        auto& prefix = node->path;

        if (path.size() < prefix.size() ||
            path.substr(0, prefix.size()) != prefix)
        {
            goto check_fallback;
        }
        path = path.substr(prefix.size());

        if (path.empty()) {
            if (node->HasHandler() || node->handler_any)
                return node;
            if (node->isPrefixMatch)
                return node;
            return (prefixFallback && prefixFallback->HasHandler())
                       ? prefixFallback
                       : nullptr;
        }

        if (node->isPrefixMatch && node->HasHandler())
            prefixFallback = node;

        {
            char c = path[0];
            for (size_t i = 0; i < node->indices.size(); ++i) {
                if (c == node->indices[i]) {
                    node = node->children[i].get();
                    goto match_child;
                }
            }
        }

        if (node->paramChild) {
            auto slash = path.find('/');
            auto seg   = (slash == std::string_view::npos)
                             ? path
                             : path.substr(0, slash);

            if (!seg.empty()) {
                if (params)
                    params->emplace_back(node->paramChild->paramName, seg);
                if (slash == std::string_view::npos) {
                    return node->paramChild;
                }
                path = path.substr(slash);
                node = node->paramChild;

                if (!node->indices.empty()) {
                    char c2 = path[0];
                    for (size_t i = 0; i < node->indices.size(); ++i) {
                        if (c2 == node->indices[i]) {
                            node = node->children[i].get();
                            goto match_child;
                        }
                    }
                }
                if (node->HasHandler())
                    return node;
                goto check_fallback;
            }
        }

    check_fallback:
        if (node->catchAllChild) {
            if (params)
                params->emplace_back(node->catchAllChild->paramName, path);
            return node->catchAllChild;
        }

        return (prefixFallback && prefixFallback->HasHandler())
                   ? prefixFallback
                   : nullptr;

    match_child:
        continue;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Match（公开匹配接口）
// ═══════════════════════════════════════════════════════════════════

// 剥离 query string（'?' 之后），仅用于路由匹配
// 参数：path - 请求路径；返回去除 query 后的路径
std::string_view Router::StripQuery(std::string_view path)
{
    auto q = path.find('?');
    return q == std::string_view::npos ? path : path.substr(0, q);
}

// 按路径匹配 handler（任意方法，向后兼容）
// 参数：path - 请求路径；params - 可选，接收捕获的路径参数；返回命中的 handler 或 nullptr
RequestHandler* Router::Match(
    std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* params) const
{
    std::shared_lock lock(rw_mutex_);
    auto* node = Lookup(root_.get(), StripQuery(path), params);
    if (!node) return nullptr;
    if (node->handler_any) return node->handler_any;
    return node->handler_get ? node->handler_get
         : node->handler_post ? node->handler_post
         : nullptr;
}

// 按 HTTP 方法 + 路径匹配 handler
// 参数：method - HTTP 方法；path - 请求路径；params - 可选，接收捕获的路径参数；返回命中的 handler 或 nullptr
RequestHandler* Router::Match(
    std::string_view method,
    std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* params) const
{
    std::shared_lock lock(rw_mutex_);
    auto* node = Lookup(root_.get(), StripQuery(path), params);
    if (!node) return nullptr;

    auto* h = node->GetHandler(method);
    if (h) return h;
    return node->handler_any;
}
