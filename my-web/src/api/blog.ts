// 数据层：前台/后台统一的唯一数据入口
// USE_MOCK=true 返回本地 mock（本地演示）；false 走真实后端（webcpp-engine，见 http.ts）
import { http } from './http'
import { mockArticles, mockCategories, mockResources } from './mock/blog'

const USE_MOCK = false

export interface Article {
  id: string
  title: string
  summary: string
  category: string
  tags?: string[]
  date: string
  cover?: string
  author?: string
  content?: string // md 文本，详情页走 MdViewer
  fileUrl?: string // md 文件地址，详情页走 MdPage
}

export interface Category {
  id: string
  name: string
}

export interface Resource {
  id: string
  name: string
  // 列表接口不下发 url（大文件是 data URL，全量返回会让列表卡死）；
  // 需要完整地址时用 getResourceUrl(id) 按需拉取
  url?: string
  type: string
}

export interface User {
  id: string
  username: string
  password?: string // 仅新增/编辑时携带；列表接口不下发密码
  createdAt?: string
}

// 站点配置：后台 SiteSettings 写入 localStorage.blogConfig，前台读取
export interface BlogConfig {
  siteName: string
  slogan: string
  copyright: string
  background: string // CSS background 值或图片 URL，留空走默认
  navMenus: { label: string; path: string }[]
}

// 站点全局配置（后端 /api/site-config 存取）：在 BlogConfig 基础上增加工作区子栏。
// 后端把整个对象存 JSON 字符串到 blog_config 表，所有设备读同一份。
export interface SiteConfig extends BlogConfig {
  workItems: { label: string; index: string; path: string }[]
}

// 模拟网络延迟，贴近真实接口体验（仅 mock 分支使用）
const delay = (ms = 200) => new Promise((r) => setTimeout(r, ms))

// 登录结果：成功后返回 token（后端签发）
export interface LoginResult {
  token: string
  username: string
}

// mock 阶段的演示账号（后端由 users 表校验）
const MOCK_USER = { username: 'admin', password: '123456' }

// 登录：POST /api/login，校验用户名密码，返回 token
export const login = async (username: string, password: string): Promise<LoginResult> => {
  if (USE_MOCK) {
    await delay()
    if (username === MOCK_USER.username && password === MOCK_USER.password) {
      return { token: `mock-token-${Date.now()}`, username }
    }
    throw new Error('用户名或密码错误')
  }
  return http.post<LoginResult>('/login', { username, password })
}

// 按分类过滤文章
export const getArticles = async (params?: { category?: string }): Promise<Article[]> => {
  if (USE_MOCK) {
    await delay()
    const list = params?.category
      ? mockArticles.filter((a) => a.category === params.category)
      : mockArticles
    return list.map((a) => ({ ...a }))
  }
  const query = params?.category
    ? `/articles?category=${encodeURIComponent(params.category)}`
    : '/articles'
  return http.get<Article[]>(query)
}

// 文章详情：404 时返回 null（与 mock 语义一致）
export const getArticleById = async (id: string): Promise<Article | null> => {
  if (USE_MOCK) {
    await delay()
    const article = mockArticles.find((a) => a.id === id)
    return article ? { ...article } : null
  }
  return http.getOptional<Article>(`/articles/${encodeURIComponent(id)}`)
}

export const getCategories = async (): Promise<Category[]> => {
  if (USE_MOCK) {
    await delay()
    return mockCategories.map((c) => ({ ...c }))
  }
  return http.get<Category[]>('/categories')
}

// 后台：文章/分类 CRUD、资源
export const saveArticle = async (article: Article): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    mockArticles.push({ ...article })
    return
  }
  await http.post<void>('/articles', article)
}

export const deleteArticle = async (id: string): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    const i = mockArticles.findIndex((a) => a.id === id)
    if (i !== -1) mockArticles.splice(i, 1)
    return
  }
  await http.delete<void>(`/articles/${encodeURIComponent(id)}`)
}

export const saveCategory = async (cat: Category): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    mockCategories.push({ ...cat })
    return
  }
  await http.post<void>('/categories', cat)
}

export const deleteCategory = async (id: string): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    const i = mockCategories.findIndex((c) => c.id === id)
    if (i !== -1) mockCategories.splice(i, 1)
    return
  }
  await http.delete<void>(`/categories/${encodeURIComponent(id)}`)
}

// 图片上传：multipart → 后端返回持久 URL（data URL，入库 resources 表）。
// onProgress 可选：0-100 上传进度回调（资源管理进度条用）
export const uploadImage = async (
  file: File,
  onProgress?: (percent: number) => void
): Promise<{ url: string }> => {
  if (USE_MOCK) {
    await delay()
    // 临时用本地预览 URL 让流程跑通
    return { url: URL.createObjectURL(file) }
  }
  return http.upload<{ url: string }>('/upload/image', file, onProgress)
}

// Markdown 上传：multipart → 后端返回持久 data URL（入库 resources 表，type='md'）。
// 保存文章时把 url 写入 article.fileUrl，详情页 MdPage fetch(dataUrl) 直接渲染。
export const uploadMarkdown = async (file: File): Promise<{ url: string; name: string }> => {
  if (USE_MOCK) {
    await delay()
    return { url: URL.createObjectURL(file), name: file.name }
  }
  return http.upload<{ url: string; name: string }>('/upload/md', file)
}

export const getResources = async (): Promise<Resource[]> => {
  if (USE_MOCK) {
    await delay()
    return mockResources.map((r) => ({ ...r }))
  }
  return http.get<Resource[]>('/resources')
}

// 按 id 取资源完整 url：列表接口不含 url，复制地址/选背景等按需拉取（GET /api/resources/:id）
export const getResourceUrl = async (id: string): Promise<string> => {
  if (USE_MOCK) {
    await delay()
    return mockResources.find((r) => r.id === id)?.url || ''
  }
  const res = await http.get<Resource>(`/resources/${encodeURIComponent(id)}`)
  return res.url || ''
}

export const deleteResource = async (id: string): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    const i = mockResources.findIndex((r) => r.id === id)
    if (i !== -1) mockResources.splice(i, 1)
    return
  }
  await http.delete<void>(`/resources/${encodeURIComponent(id)}`)
}

// ═══════════════════════════════════════════════════════════════
// 用户管理（后台「系统管理 → 用户管理」）
// ═══════════════════════════════════════════════════════════════
const MOCK_USERS: User[] = [
  { id: '1', username: 'admin', password: '123456', createdAt: '2026-01-01 00:00:00' }
]

export const getUsers = async (): Promise<User[]> => {
  if (USE_MOCK) {
    await delay()
    return MOCK_USERS.map((u) => ({ ...u }))
  }
  return http.get<User[]>('/users')
}

export const saveUser = async (user: User): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    const i = MOCK_USERS.findIndex((u) => u.id === user.id && user.id)
    if (i !== -1) {
      MOCK_USERS[i] = { ...MOCK_USERS[i], ...user }
    } else {
      MOCK_USERS.push({ ...user, id: String(MOCK_USERS.length + 1) })
    }
    return
  }
  await http.post<void>('/users', user)
}

export const deleteUser = async (id: string): Promise<void> => {
  if (USE_MOCK) {
    await delay()
    const i = MOCK_USERS.findIndex((u) => u.id === id)
    if (i !== -1) MOCK_USERS.splice(i, 1)
    return
  }
  await http.delete<void>(`/users/${encodeURIComponent(id)}`)
}

// 站点访问统计（/api/stats，数据来自后端分钟级落库 site_stats）
export interface StatsPoint {
  ts: string
  req: number
  err: number
  bytes: number
  p50: number
  p90: number
  p99: number
  act: number
}
export interface StatsResult {
  range: string
  total: { req: number; err: number; bytes: number; p99_max: number }
  points: StatsPoint[]
}
export const getStats = async (range: '24h' | '7d' = '24h'): Promise<StatsResult> => {
  if (USE_MOCK) {
    return { range, total: { req: 0, err: 0, bytes: 0, p99_max: 0 }, points: [] }
  }
  return http.get<StatsResult>(`/stats?range=${range}`)
}

// ── 局域网访问开关（后台「工作区 → 局域网访问」） ──
// 开启后同一 WiFi 下设备可通过 http://<lanIp>:<port> 访问；关闭后返回 403
export interface LanStatus {
  enabled: boolean
  lanIp: string
}
export const getLanStatus = async (): Promise<LanStatus> => {
  if (USE_MOCK) return { enabled: false, lanIp: '' }
  return http.get<LanStatus>('/network/lan')
}
export const setLanEnabled = async (enabled: boolean): Promise<LanStatus> => {
  if (USE_MOCK) return { enabled, lanIp: '' }
  return http.post<LanStatus>('/network/lan', { enabled })
}

// ── 访问者 IP 统计（仪表盘展示；数据来自宿主机转发器 lan-proxy 上报真实对端 IP） ──
export interface VisitorInfo {
  ip: string
  cnt: number
  firstSeen: string
  lastSeen: string
  online: boolean
}
export interface VisitorResult {
  onlineCount: number
  visitors: VisitorInfo[]
}
export const getVisitors = async (limit = 20): Promise<VisitorResult> => {
  if (USE_MOCK) return { onlineCount: 0, visitors: [] }
  return http.get<VisitorResult>(`/network/visitor?limit=${limit}`)
}

// ── 站点全局配置（后台「站点设置」→ 后端 blog_config 表，所有设备共享） ──
// GET /api/site-config：后端有配置返回 {config: SiteConfig}，首次无记录返回 {config: null}
export const getSiteConfig = async (): Promise<{ config: SiteConfig | null }> => {
  if (USE_MOCK) return { config: null }
  return http.get<{ config: SiteConfig | null }>('/site-config')
}
// POST /api/site-config：body 直接传整个配置对象，后端原样存 JSON 字符串
export const saveSiteConfig = async (config: SiteConfig): Promise<void> => {
  if (USE_MOCK) return
  await http.post<void>('/site-config', config)
}
