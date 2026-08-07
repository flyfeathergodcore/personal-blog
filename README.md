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
├── my-web/               # Vue 3 前端
│   ├── src/              # 组件 / 页面 / 路由 / API / 组合式函数
│   ├── public/           # 静态资源（favicon 等）
│   ├── package.json      # 前端依赖（npm）
│   └── vite.config.ts
├── webcpp-engine/        # C++ 后端服务器
│   ├── src/              # 博客业务 handler（文章 / 分类 / 资源 / 站点配置 / LAN 等）
│   ├── net/              # TCP / TLS / 缓冲读写
│   ├── protocol/         # HTTP/1.1、HTTP/2、WebSocket
│   ├── server/           # 会话 / 连接池 / 多路复用
│   ├── router/           # 路由注册
│   ├── sql/              # 数据库初始化脚本（init.sql）
│   ├── config/           # 服务配置（blog.yaml）
│   ├── Dockerfile        # 两阶段构建镜像
│   └── build-run.sh      # 一键构建 + 部署脚本
└── coro/                 # 自研 C++20 协程库（epoll / kqueue 事件循环）
```

## 🚀 构建与运行

依赖环境：Node.js（构建前端）、Docker（构建运行容器）、MySQL 8.0 容器（名称 `mysql1`）。

```bash
# 1. 初始化数据库（在 MySQL 容器 mysql1 中执行）
docker exec -i mysql1 mysql -uroot -p123456 < webcpp-engine/sql/init.sql

# 2. 一键构建 + 启动（内部流程：npm build 前端 → docker build 镜像 → 启动容器 → 启动局域网转发器）
cd webcpp-engine && ./build-run.sh

# 3. 访问
#    本机    : http://localhost:8443
#    局域网   : 后台「工作区设置」开启局域网访问后，手机 / 平板等同一 WiFi 设备访问
```

> 说明：镜像构建 context 需要包含 `mysql_connection_pool`（后端连接池依赖的独立源码库），
> 见 `webcpp-engine/Dockerfile` 注释。

## 📊 性能参考

webcpp-engine 后端与 nginx 1.24 实测对比（4 核 Linux，各 4 worker）：

- **HTTP/1.1 明文**：低并发（c=200）与 nginx 打平（反超至 102%），高并发（c=1000）达 92%
- **HTTP/2 TLS**：**为 nginx 的 2.0 倍**（c=200, m=10 多路复用）

详见 `webcpp-engine/README.md`。

## 📄 License

本项目仅供学习交流使用。
