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
# 构建上下文 = 本仓库根目录（vue-web/personal-blog，含 mysql_connection_pool + coro
#   + webcpp-engine + my-web 全部源码）
#   构建：cd my-web && npm run build && docker build -f webcpp-engine/Dockerfile -t ... .
#   或直接：docker compose up -d --build（见仓库根 docker-compose.yml）
# ═══════════════════════════════════════════════════════════════════

# ── 阶段 1：C++ 后端 ──
FROM ubuntu:22.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
# 清空 buildkit 注入的宿主机代理（host.docker.internal:7890 在容器内连不通），apt 直连
ENV http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= all_proxy= ALL_PROXY=
# gcc-12：协程 ABI 必须全工程统一同一编译器——GCC -fcoroutines 与 clang 标准
# C++20 协程 ABI 互不混链（混链会在运行时崩溃），构建容器固定用 GCC；
# GCC 11 + -O3 + -fcoroutines 有协程优化卡死 bug，GCC 12 已修复。
RUN apt-get update && apt-get install -y --no-install-recommends -o Acquire::Retries=3 \
        build-essential cmake pkg-config ca-certificates gcc-12 g++-12 \
        libssl-dev libyaml-cpp-dev libmysqlclient-dev libnghttp2-dev libmimalloc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# coro 协程库统一使用 vue-web/coro（连接池、引擎共用同一份，避免 coro
# EventLoop TLS 双实例）；顶层 CMakeLists 统一编排整体构建
COPY CMakeLists.txt /src/CMakeLists.txt
COPY mysql_connection_pool/ /src/mysql_connection_pool/
COPY webcpp-engine/ /src/webcpp-engine/
# 关闭在镜像内跑 git 相关的问题：直接 cmake 配置 + 构建 demo_server
# 必须用 GCC（-fcoroutines ABI）：协程 ABI 必须全工程统一——clang 标准 C++20
# ABI 与 GCC -fcoroutines ABI 互不混链，混链在运行时崩溃。GCC 11 卡死，故用 GCC 12。
# -DWEBCPP_RPC=OFF：跳过尚未完成的 rpc 开发模块（依赖 protobuf，构建容器未装；
#   与现网镜像一致——现网即不含 rpc）。RPC 完成后移除该开关并补 COPY rpc/ + protobuf 依赖。
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++-12 \
        -DWEBCPP_RPC=OFF \
    && cmake --build build -j2 --target demo_server

FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
# 同 build 阶段：清空代理，apt 直连
ENV http_proxy= https_proxy= HTTP_PROXY= HTTPS_PROXY= all_proxy= ALL_PROXY=
RUN apt-get update && apt-get install -y --no-install-recommends -o Acquire::Retries=3 \
        libssl3 libyaml-cpp0.7 libmysqlclient21 libmimalloc2.0 libgcc-s1 libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/webcpp-engine/demo_server /app/demo_server
# 前端静态产物（宿主机 npm run build 产出），由网关 doc_root=/app/www 直接 serve，
# 浏览器同源访问 :8443，/api 无需代理
COPY my-web/dist/ /app/www/
COPY webcpp-engine/http/config/blog.yaml /app/config.yaml
EXPOSE 8443
CMD ["./demo_server", "-c", "/app/config.yaml"]
