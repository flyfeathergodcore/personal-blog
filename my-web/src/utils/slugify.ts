// src/utils/slugify.ts Markdown 标题 → 锚点 id 的纯函数
// 抽成独立模块：便于单元测试与回归验证（MdViewer 内部标题锚点、OutlineAnchor 点击定位共用）
/**
 * slugify：标题文本转锚点 id（保留中文，其余转小写、空格转连字符）
 *
 * 关键约束：生成的 id 会被当作 CSS 选择器使用（OutlineAnchor 点击时
 * document.querySelector('#id')、el-anchor 内部 getElement 滚动定位）。
 * CSS 标识符不能以数字开头，否则 querySelector 抛 SyntaxError、点击逻辑中断、
 * 默认 <a href> 跳转污染 hash 路由 → 锚点失效。
 * 因此以数字开头的 slug 自动加 'md-' 前缀，保证 id 恒为合法 CSS id。
 *
 * @param text 标题原始文本
 * @returns 生成的锚点 id（合法 CSS id）；空文本兜底返回 'section'
 */
export const slugify = (text: string): string => {
  const slug =
    text
      .trim()
      .toLowerCase()
      .replace(/[^\w一-龥]+/g, '-')
      .replace(/^-+|-+$/g, '') || 'section'
  // CSS 标识符禁止数字开头（如 '1-xxx' 会让 querySelector 抛 SyntaxError）→ 加字母前缀兜底
  return /^[0-9]/.test(slug) ? `md-${slug}` : slug
}
