// mock 数据：后台接口就绪前的演示数据源（USE_MOCK=true 时使用）
import type { Article, Category, Resource } from '../blog'

// 示例文章（content 为 md 文本，详情页走 MdViewer 渲染）
export const mockArticles: Article[] = [
  {
    id: '1',
    title: 'Markdown 渲染与锚点导航实践',
    summary: '介绍 markdown-it + highlight.js + DOMPurify 组合的渲染管线，以及标题锚点与大纲导航的实现思路。',
    category: '前端',
    tags: ['markdown', 'vue'],
    date: '2026-08-01',
    author: '示例作者',
    content: `# Markdown 渲染与锚点导航实践

## 渲染管线

- \`markdown-it\` 负责解析与渲染
- \`highlight.js\` 处理代码高亮
- \`DOMPurify\` 白名单清洗防 XSS

## 锚点导航

标题自动生成 id，配合 el-anchor 实现大纲跳转。

\`\`\`ts
const slugify = (text: string): string => text.toLowerCase()
\`\`\`
`
  },
  {
    id: '2',
    title: '博客前台组件设计思路',
    summary: '从组件划分、数据驱动、CSS 变量主题三个角度，梳理博客前台与后台控制的协作方式。',
    category: '架构',
    tags: ['vue', '架构'],
    date: '2026-07-28',
    author: '示例作者',
    content: `# 博客前台组件设计思路

## 组件划分

每个首页区块对应一个独立组件，便于后期后台逐个配置风格。
`
  },
  {
    id: '3',
    title: '前后台联动与配置持久化',
    summary: '后台设置通过 localStorage 持久化，前台挂载时读取，实现刷新即生效的配置联动。',
    category: '架构',
    tags: ['配置'],
    date: '2026-07-20',
    author: '示例作者',
    content: `# 前后台联动与配置持久化

后台写入 \`localStorage.blogConfig\`，前台读取，刷新即生效。
`
  },
  {
    id: '4',
    title: 'Element Plus 暗色模式接入',
    summary: '通过 html.dark class 与 CSS 变量实现日/夜切换，顺带启用 Element Plus 暗色变量。',
    category: '前端',
    tags: ['element-plus'],
    date: '2026-07-15',
    author: '示例作者',
    content: `# Element Plus 暗色模式接入

切换 \`html.dark\` class，配合自定义 CSS 变量与 Element Plus dark css-vars。
`
  }
]

// 分类
export const mockCategories: Category[] = [
  { id: '1', name: '前端' },
  { id: '2', name: '架构' }
]

// 资源（图片库，占位图服务）
export const mockResources: Resource[] = [
  { id: '1', name: '封面-山川', url: 'https://picsum.photos/seed/blog-1/800/450', type: 'image' },
  { id: '2', name: '封面-海洋', url: 'https://picsum.photos/seed/blog-2/800/450', type: 'image' },
  { id: '3', name: '背景-森林', url: 'https://picsum.photos/seed/blog-3/1920/1080', type: 'image' }
]
