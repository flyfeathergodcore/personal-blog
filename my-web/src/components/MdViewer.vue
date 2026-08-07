<!-- src/components/MdViewer.vue Markdown 展示组件：内容由外部传入，内部负责解析渲染
     同时为标题生成锚点 id，并通过 anchors 事件通知父组件（供大纲锚点组件使用） -->
<template>
  <div class="md-viewer" v-html="renderedHtml"></div>
</template>

<script lang="ts" setup>
import { ref, watch } from 'vue'
import MarkdownIt from 'markdown-it'
import hljs from 'highlight.js'
import DOMPurify from 'dompurify'
import 'highlight.js/styles/github.css'

// 锚点数据结构：id 对应标题元素 id（供 el-anchor 跳转），level 为标题层级 h1=1
export interface AnchorItem {
  id: string
  text: string
  level: number
}

// props：content 为 Markdown 原文，由外部传入（可来自上传、接口、静态文件）
const props = defineProps({
  content: {
    type: String,
    default: ''
  }
})

// 组件事件：解析出的锚点列表通知父组件
const emit = defineEmits(['anchors'])

// markdown-it 实例：启用 GFM（表格/删除线/自动链接），代码块用 highlight.js 高亮
const md = new MarkdownIt({
  html: true, // 透传内嵌 HTML（支持含 HTML 的 md）；XSS 由下方 DOMPurify 白名单清洗兜底
  linkify: true, // 自动识别裸 URL 并转为链接
  // 返回类型显式标注为 string，打破与 md 的循环类型推断
  highlight: (code: string, lang: string): string => {
    // 能识别语言则高亮，否则仅转义输出（防止注入）
    if (lang && hljs.getLanguage(lang)) {
      return `<pre class="hljs"><code>${hljs.highlight(code, { language: lang }).value}</code></pre>`
    }
    return `<pre class="hljs"><code>${md.utils.escapeHtml(code)}</code></pre>`
  }
})

// 渲染结果（清洗后）与锚点列表
const renderedHtml = ref('')
const anchors = ref<AnchorItem[]>([])

// 标题 id 计数表：同名标题追加后缀（-2、-3...），保证 id 唯一
const headingIdCounts = new Map<string, number>()

// slugify：标题文本 -> 锚点 id（保留中文，其余转小写/空格转连字符）
const slugify = (text: string): string =>
  text
    .trim()
    .toLowerCase()
    .replace(/[^\w一-龥]+/g, '-')
    .replace(/^-+|-+$/g, '') || 'section'

// 生成唯一标题 id
const uniqueId = (text: string): string => {
  const base = slugify(text)
  const count = headingIdCounts.get(base) ?? 0
  headingIdCounts.set(base, count + 1)
  return count === 0 ? base : `${base}-${count + 1}`
}

// 渲染管线：md.parse 解析 token 流（收集锚点 + 为标题注入 id）
//   -> md.renderer.render 输出 HTML -> DOMPurify 白名单清洗（防 XSS）-> v-html 输出
// DOMPurify 默认只保留安全标签/属性，自动移除 script、iframe、事件属性、javascript: 协议等
const renderMd = () => {
  const tokens = md.parse(props.content, {})
  const list: AnchorItem[] = []
  headingIdCounts.clear() // 每次渲染重置，保证 id 生成一致

  // 遍历 token 流：heading_open 后紧跟 inline 内容 token
  for (let i = 0; i < tokens.length; i++) {
    const token = tokens[i]
    if (!token || token.type !== 'heading_open') continue
    const contentToken = tokens[i + 1]
    const title = contentToken?.content || ''
    const id = uniqueId(title)
    token.attrPush(['id', id]) // 注入 id 到标题元素
    list.push({
      id,
      text: title,
      level: Number(token.tag.slice(1)) // h1 -> 1
    })
  }

  anchors.value = list
  emit('anchors', list)
  renderedHtml.value = DOMPurify.sanitize(md.renderer.render(tokens, md.options, {}))
}

// content 变化时重新渲染；immediate 让挂载时立即渲染
watch(() => props.content, renderMd, { immediate: true })
</script>

<style scoped>
/* Markdown 排版样式：覆盖标题/列表/引用/代码/表格/图片等 */
.md-viewer {
  font-size: 14px;
  line-height: 1.8;
  color: var(--el-text-color-primary);
  word-break: break-word;
}

/* 标题 */
.md-viewer :deep(h1),
.md-viewer :deep(h2),
.md-viewer :deep(h3),
.md-viewer :deep(h4) {
  margin: 24px 0 12px;
  font-weight: 600;
  line-height: 1.4;
  /* 锚点跳转时标题不贴顶 */
  scroll-margin-top: 70px;
}

.md-viewer :deep(h1) {
  font-size: 26px;
  padding-bottom: 8px;
  border-bottom: 1px solid var(--el-border-color);
}

.md-viewer :deep(h2) {
  font-size: 22px;
  padding-bottom: 6px;
  border-bottom: 1px solid #f0f0f0;
}

.md-viewer :deep(h3) {
  font-size: 18px;
}

.md-viewer :deep(h4) {
  font-size: 15px;
}

/* 段落与列表 */
.md-viewer :deep(p) {
  margin: 8px 0;
}

.md-viewer :deep(ul),
.md-viewer :deep(ol) {
  margin: 8px 0;
  padding-left: 24px;
}

.md-viewer :deep(li) {
  margin: 4px 0;
}

/* 引用块 */
.md-viewer :deep(blockquote) {
  margin: 12px 0;
  padding: 8px 16px;
  color: var(--el-text-color-regular);
  background: #f8f8f9;
  border-left: 4px solid #409eff;
  border-radius: 0 4px 4px 0;
}

/* 行内代码与代码块 */
.md-viewer :deep(code) {
  padding: 2px 6px;
  font-family: 'SF Mono', Consolas, Menlo, monospace;
  font-size: 13px;
  background: var(--el-fill-color-light);
  border-radius: 4px;
}

.md-viewer :deep(pre) {
  margin: 12px 0;
  padding: 12px 16px;
  background: #f6f8fa;
  border-radius: 6px;
  overflow-x: auto;
}

/* pre 内 code 不再加行内样式（highlight.js 已处理配色） */
.md-viewer :deep(pre code) {
  padding: 0;
  background: transparent;
  border-radius: 0;
}

/* 表格 */
.md-viewer :deep(table) {
  width: 100%;
  margin: 12px 0;
  border-collapse: collapse;
  font-size: 13px;
}

.md-viewer :deep(th),
.md-viewer :deep(td) {
  padding: 8px 12px;
  text-align: left;
  border: 1px solid var(--el-border-color);
}

.md-viewer :deep(th) {
  background: var(--el-fill-color-light);
  font-weight: 600;
}

/* 图片与链接 */
.md-viewer :deep(img) {
  max-width: 100%;
  border-radius: 4px;
}

.md-viewer :deep(a) {
  color: #409eff;
  text-decoration: none;
}

.md-viewer :deep(a:hover) {
  text-decoration: underline;
}

/* 分割线 */
.md-viewer :deep(hr) {
  margin: 20px 0;
  border: none;
  border-top: 1px solid var(--el-border-color);
}
</style>
