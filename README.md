# 📝 个人博客（Personal Blog）

一个自部署的个人博客系统：**Vue 3 前端 + C++ 协程 HTTP 后端 + MySQL**，支持前台文章浏览、后台全功能管理、局域网内多设备共享访问。

## ✨ 功能特性

- **前台**：文章列表 / 分类浏览 / Markdown 文章详情（目录锚点）、主页导航菜单、站点名称/标语/版权/背景可配、夜间模式（跟随系统 + 手动切换）
- **后台**：文章 / 分类 / 资源管理、Markdown 上传与预览、菜单管理、站点设置、工作区子栏、用户管理、仪表盘访问统计
- **站点配置全局化**：站点名称、导航菜单、主页背景等配置存于后端 MySQL（`blog_config` 表），局域网内所有设备读取同一份配置，一处修改全局生效
- **局域网共享访问**：同一 WiFi 下设备可通过局域网地址访问网站，后端按 Host 头控制开关
- **AI 助手**：内置对话助手组件
- **夜间模式**：所有后台面板与前台内容页均适配 Element Plus 暗色变量

## 🛠️ 技术栈

| 层 | 技术 |
|---|---|
| 前端 | Vue 3 + TypeScript + Vite + Element Plus |
| 后端 | webcpp-engine：基于 coroutine 的 C++ HTTP/1.1 + HTTP/2 服务器（零 asio 依赖） |
| 协程库 | coro：自研 C++20 协程事件循环（epoll ET，每 worker 独立 EventLoop） |
| 数据库 | MySQL 8.0（Docker 容器） |
| 部署 | Docker（两阶段构建，Ubuntu 22.04 基础镜像）+ docker-compose 式自定义网络 |

## 📁 目录结构

```
.
├── my-web/                # Vue 3 前端
│   ├── src/               # 组件 / 页面 / 路由 / API / 组合式函数
│   ├── public/            # 静态资源（favicon 等）
│   ├── package.json       # 前端依赖（npm）
│   └── vite.config.ts
├── webcpp-engine/         # C++ 后端服务器
│   ├── src/               # 博客业务 handler（文章 / 分类 / 资源 / 站点配置 / LAN 等）
│   ├── net/               # TCP / TLS / 缓冲读写
│   ├── protocol/          # HTTP/1.1、HTTP/2、WebSocket
│   ├── server/            # 会话 / 连接池 / 多路复用
│   ├── router/            # 路由注册
│   ├── sql/               # 数据库初始化脚本（init.sql）
│   ├── config/            # 服务配置（blog.yaml）
│   ├── Dockerfile         # 两阶段构建镜像
│   └── build-run.sh       # 一键构建 + 部署脚本（跨平台）
├── mysql_connection_pool/ # MySQL 异步连接池（C++ 协程，含内部 coro 副本）
└── coro/                  # 自研 C++20 协程库（epoll / kqueue 事件循环）
```

## 🚀 Docker 构建与运行（macOS / Linux / Windows）

依赖环境（三平台通用）：**Node.js 18+**（构建前端）、**Docker**（构建运行容器）、**MySQL 8.0 容器**（名称 `mysql1`）。

### 1. 按平台安装 Docker 与 bash

| 平台 | Docker 安装 | 执行脚本的 bash 环境 |
|---|---|---|
| macOS | [Docker Desktop](https://www.docker.com/products/docker-desktop/) | 系统自带 |
| Linux | Docker Engine（`apt install docker.io` 或官方源） | 系统自带 |
| Windows | [Docker Desktop](https://www.docker.com/products/docker-desktop/)（启用 WSL2 后端） | [Git Bash](https://git-scm.com/)（Git for Windows）或 WSL2 |

前端构建（`npm install && npm run build`）三个平台均原生支持，无需额外环境。

### 2. 部署步骤（三个平台完全一致）

```bash
# ① 启动 MySQL 容器，并加入自定义网络（后端按容器名 mysql1 互连）
docker run -d --name mysql1 -e MYSQL_ROOT_PASSWORD=123456 mysql:8.0
docker network create blog-net
docker network connect blog-net mysql1

# ② 初始化数据库
docker exec -i mysql1 mysql -uroot -p123456 < webcpp-engine/sql/init.sql

# ③ 构建前端
cd my-web && npm install && npm run build

# ④ 一键构建镜像 + 启动容器 + 启动局域网转发器
cd webcpp-engine && ./build-run.sh

# ⑤ 访问
#    本机    : http://localhost:8443
#    局域网   : 后台「工作区设置」开启局域网访问后，同一 WiFi 下的设备访问
```

> **说明**
> - `build-run.sh` 已跨平台：局域网 IP 自动探测支持 macOS / Linux / WSL2 / Windows（Git Bash），
>   Python 解释器自动探测 `python3` / `python`。Windows 下用 Git Bash 或 WSL2 执行即可，**步骤与上完全一致**。
> - `mysql_connection_pool`（MySQL 异步连接池）与 `coro`（协程库）均已包含在本仓库；镜像构建 context
>   需为仓库的**上级目录**（Dockerfile 的 COPY 路径为 `vue-web/...`），见 `webcpp-engine/Dockerfile` 注释。

### 3. 平台差异速查

| 场景 | 说明 |
|---|---|
| Windows 局域网共享 | Docker Desktop 端口映射到 Windows 主机，局域网设备直接访问 Windows 局域网 IP 即可 |
| WSL2 内直接跑服务（不走 Docker） | 需 `netsh interface portproxy` 转发或 WSL 镜像网络模式，局域网设备才能直连 |
| 局域网 IP 自动探测失败（如多网卡取错） | 手动指定：`export HOST_LAN_IP="192.168.x.x"` 后再执行 `./build-run.sh` |

### 4. 可选：WSL2 / Linux 内直接编译运行（不走 Docker）

WSL2 提供完整 Linux 内核，epoll 与 GCC 12 均可用：

```bash
# 1. 安装编译依赖
sudo apt update && sudo apt install -y gcc-12 g++-12 cmake \
  libmysqlclient-dev libssl-dev libyaml-cpp-dev

# 2. 构建前端
cd my-web && npm install && npm run build

# 3. 准备本地运行配置（仓库内 blog.yaml 为 Docker 容器专用，需改 doc_root 与 MySQL 地址）
cat > webcpp-engine/build/local.yaml <<'EOF'
server:
  host: 0.0.0.0
  port: 8443
  threads: 4
  doc_root: /absolute/path/to/my-web/dist
  log_dir: /tmp/blog-logs
  log_level: info
mysql:
  host: 127.0.0.1
  port: 3306
  user: root
  password: "123456"
  database: blog_db
  min_size: 4
  max_size: 16
EOF

# 4. 编译后端
cmake -S webcpp-engine -B webcpp-engine/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-12 \
  -DCORO_DIR=$(pwd)/coro -DMYSQL_POOL_DIR=$(pwd)/mysql_connection_pool
cmake --build webcpp-engine/build -j --target demo_server

# 5. 运行
./webcpp-engine/build/demo_server -c webcpp-engine/build/local.yaml
```

> 注：MySQL 需让 `127.0.0.1:3306` 可达（如 `docker run -d --name mysql1 -p 3306:3306 -e MYSQL_ROOT_PASSWORD=123456 mysql:8.0`）。

### 5. 原生编译限制

- **事件循环**：`coro` 协程库只有 epoll（Linux）/ kqueue（macOS）实现，**没有 Windows IOCP**，Windows 不支持原生编译，必须走 Docker / WSL2；
- **协程 ABI**：后端用 GCC `-fcoroutines` 专用 ABI（MSVC / clang 不兼容），需 GCC 12；
- **系统调用**：依赖 `<sys/epoll.h>` / `<sys/event.h>` 等 POSIX 头文件。

前端 `my-web`（Vue/Vite）完全跨平台，可在任意平台构建。

## 📊 性能参考

webcpp-engine 后端与 nginx 1.24 实测对比（4 核 Linux，各 4 worker）：

- **HTTP/1.1 明文**：低并发（c=200）与 nginx 打平（反超至 102%），高并发（c=1000）达 92%
- **HTTP/2 TLS**：**为 nginx 的 2.0 倍**（c=200, m=10 多路复用）

详见 `webcpp-engine/README.md`。

## 📄 License

本项目仅供学习交流使用。
