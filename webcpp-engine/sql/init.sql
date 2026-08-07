-- blog_db 初始化：建库建表 + 种子数据（webcpp-engine 博客后端）
-- 说明：NO_BACKSLASH_ESCAPES 关闭反斜杠转义，md 正文里的 \ 反引号原样入库
SET NAMES utf8mb4;
SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES';

DROP DATABASE IF EXISTS blog_db;
CREATE DATABASE blog_db CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE blog_db;

-- 用户表
CREATE TABLE users (
  id INT AUTO_INCREMENT PRIMARY KEY,
  username VARCHAR(50) NOT NULL UNIQUE,
  password VARCHAR(100) NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- 分类表
CREATE TABLE categories (
  id INT AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(50) NOT NULL UNIQUE
) ENGINE=InnoDB;

-- 文章表（content 为 md 文本，file_url 备用）
CREATE TABLE articles (
  id INT AUTO_INCREMENT PRIMARY KEY,
  title VARCHAR(200) NOT NULL,
  summary VARCHAR(500) NOT NULL DEFAULT '',
  category VARCHAR(50) NOT NULL DEFAULT '',
  tags VARCHAR(200) NOT NULL DEFAULT '',      -- 逗号分隔
  date VARCHAR(20) NOT NULL DEFAULT '',
  cover VARCHAR(500) NOT NULL DEFAULT '',
  author VARCHAR(50) NOT NULL DEFAULT '',
  content MEDIUMTEXT,
  -- data URL 存 md 文档，base64 后可达数 KB，VARCHAR 放不下，必须 MEDIUMTEXT
  file_url MEDIUMTEXT NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- 资源表
CREATE TABLE resources (
  id INT AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(200) NOT NULL,
  -- 上传文件以 base64 data URL 入库（图片 / md），VARCHAR(1000) 放不下大文件，必须 MEDIUMTEXT
  url MEDIUMTEXT NOT NULL,
  type VARCHAR(50) NOT NULL DEFAULT ''
) ENGINE=InnoDB;

-- 种子：管理员账号 admin / 123456（明文，demo 用）
INSERT INTO users (username, password) VALUES ('admin', '123456');

-- 种子：分类
INSERT INTO categories (name) VALUES ('前端'), ('架构');

-- 种子：资源（占位图服务）
INSERT INTO resources (name, url, type) VALUES
('封面-山川', 'https://picsum.photos/seed/blog-1/800/450', 'image'),
('封面-海洋', 'https://picsum.photos/seed/blog-2/800/450', 'image'),
('背景-森林', 'https://picsum.photos/seed/blog-3/1920/1080', 'image');

-- 种子：文章（content 原样从 my-web/src/api/mock/blog.ts 迁移）
INSERT INTO articles (title, summary, category, tags, date, author, content) VALUES
('Markdown 渲染与锚点导航实践',
 '介绍 markdown-it + highlight.js + DOMPurify 组合的渲染管线，以及标题锚点与大纲导航的实现思路。',
 '前端', 'markdown,vue', '2026-08-01', '示例作者',
 '# Markdown 渲染与锚点导航实践

## 渲染管线

- \`markdown-it\` 负责解析与渲染
- \`highlight.js\` 处理代码高亮
- \`DOMPurify\` 白名单清洗防 XSS

## 锚点导航

标题自动生成 id，配合 el-anchor 实现大纲跳转。

\`\`\`ts
const slugify = (text: string): string => text.toLowerCase()
\`\`\`
'),
('博客前台组件设计思路',
 '从组件划分、数据驱动、CSS 变量主题三个角度，梳理博客前台与后台控制的协作方式。',
 '架构', 'vue,架构', '2026-07-28', '示例作者',
 '# 博客前台组件设计思路

## 组件划分

每个首页区块对应一个独立组件，便于后期后台逐个配置风格。
'),
('前后台联动与配置持久化',
 '后台设置通过 localStorage 持久化，前台挂载时读取，实现刷新即生效的配置联动。',
 '架构', '配置', '2026-07-20', '示例作者',
 '# 前后台联动与配置持久化

后台写入 \`localStorage.blogConfig\`，前台读取，刷新即生效。
'),
('Element Plus 暗色模式接入',
 '通过 html.dark class 与 CSS 变量实现日/夜切换，顺带启用 Element Plus 暗色变量。',
 '前端', 'element-plus', '2026-07-15', '示例作者',
 '# Element Plus 暗色模式接入

切换 \`html.dark\` class，配合自定义 CSS 变量与 Element Plus dark css-vars。
');

-- 站点访问统计表（分钟级聚合落库；新库初始化时一并创建）
-- 注意：单独维护时用 sql/init_stats.sql（CREATE TABLE IF NOT EXISTS，不会清空数据）
CREATE TABLE IF NOT EXISTS site_stats (
  id INT AUTO_INCREMENT PRIMARY KEY,
  ts DATETIME NOT NULL,            -- 分钟级时间点
  req INT NOT NULL DEFAULT 0,      -- 请求数
  req_h1 INT NOT NULL DEFAULT 0, req_h2 INT NOT NULL DEFAULT 0,
  err INT NOT NULL DEFAULT 0,      -- 错误数
  bytes BIGINT NOT NULL DEFAULT 0, -- 传输字节
  p50 INT NOT NULL DEFAULT 0, p90 INT NOT NULL DEFAULT 0, p99 INT NOT NULL DEFAULT 0,
  act INT NOT NULL DEFAULT 0,      -- 平均活动连接（四舍五入存）
  act_max INT NOT NULL DEFAULT 0,  -- 峰值活动连接
  UNIQUE KEY uk_ts (ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
