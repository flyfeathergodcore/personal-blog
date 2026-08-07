# ═══════════════════════════════════════════════════════════════════
# webcpp-engine 博客镜像（两阶段构建）
#
# 前端静态产物（my-web/dist）在宿主机执行 npm run build 产出（build-run.sh
# 会先构建前端），此处仅 COPY 进镜像；Docker 内不再跑 node，避免拉 node
# 镜像依赖外网（本机 Docker Desktop 代理 127.0.0.1:7890 未连通，拉不到新镜像）。
#
# 基础镜像选 Ubuntu（而非 alpine）：
#   连接池依赖 MySQL 8.0+ 非阻塞客户端 API（mysql_real_connect_nonblocking 等），
#   Ubuntu 的 libmysqlclient-dev 提供该 API；alpine 只有 mariadb-connector-c 不支持。
#
# 构建上下文 = /Users/Zhuanz1/Documents/github
#   （含 vue-web 仓库整体：mysql_connection_pool + vue-web/webcpp-engine + my-web）
#   构建：cd my-web && npm run build && docker build ...
# ═══════════════════════════════════════════════════════════════════

# ── 阶段 1：C++ 后端 ──
FROM ubuntu:22.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
# 清空 buildkit 注入的宿主机代理（host.docker.internal:7890 在容器内连不通），apt 直连
ENV http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= all_proxy= ALL_PROXY=
# gcc-12：webcpp 的 coro Task 库是 GCC -fcoroutines ABI（clang 不兼容，会崩）；
# GCC 11 + -O3 + -fcoroutines 有协程优化卡死 bug，GCC 12 已修复。
RUN apt-get update && apt-get install -y --no-install-recommends -o Acquire::Retries=3 \
        build-essential cmake pkg-config ca-certificates gcc-12 g++-12 \
        libssl-dev libyaml-cpp-dev libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# coro 协程库用 vue-web/coro（含 operator co_await，Task 可直接 co_await；
# 连接池 mysql_connection_pool 用同一份 coro 静态链接，避免 coro TLS 双实例）
COPY vue-web/mysql_connection_pool/ /src/mysql_connection_pool/
COPY vue-web/coro/ /src/coro/
COPY vue-web/webcpp-engine/ /src/webcpp-engine/
# 关闭在镜像内跑 git 相关的问题：直接 cmake 配置 + 构建 demo_server
# 必须用 GCC（-fcoroutines ABI）：coro Task 库为 GCC 协程 ABI 设计，clang 标准
# C++20 ABI 编译通过但运行时 this 传递不兼容（实测 SignalWatcher::wait 崩）。
# GCC 11 卡死，故用 GCC 12。
RUN cmake -S webcpp-engine -B webcpp-engine/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++-12 \
        -DCORO_DIR=/src/coro \
        -DMYSQL_POOL_DIR=/src/mysql_connection_pool \
    && cmake --build webcpp-engine/build -j2 --target demo_server

FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
# 同 build 阶段：清空代理，apt 直连
ENV http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= all_proxy= ALL_PROXY=
RUN apt-get update && apt-get install -y --no-install-recommends -o Acquire::Retries=3 \
        libssl3 libyaml-cpp0.7 libmysqlclient21 libgcc-s1 libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/webcpp-engine/build/demo_server /app/demo_server
# 前端静态产物（宿主机 npm run build 产出），由网关 doc_root=/app/www 直接 serve，
# 浏览器同源访问 :8443，/api 无需代理
# 注意：context 根是 github/，my-web 在 vue-web/ 下，路径需带 vue-web/ 前缀
COPY vue-web/my-web/dist/ /app/www/
COPY vue-web/webcpp-engine/config/blog.yaml /app/config.yaml
EXPOSE 8443
CMD ["./demo_server", "-c", "/app/config.yaml"]
