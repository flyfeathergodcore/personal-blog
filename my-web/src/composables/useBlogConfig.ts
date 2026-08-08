// 站点配置读写：后台 SiteSettings 写入、前台组件读取，localStorage 真联动
//
// 数据源分层（本次「全局化」改造）：
//   - 后端 blog_config 表（/api/site-config）是最终事实源：后台保存时写入，
//     所有设备（含局域网共享设备）读到同一份配置，删除「关于」等修改全局生效。
//   - localStorage.blogConfig 是本地缓存：首屏快速渲染 + 后端不可达时的兜底。
//   - initBlogConfig() 在启动/挂载时拉后端覆盖本地，实现「改一处、处处同步」。
import { reactive } from 'vue'
import { getSiteConfig, saveSiteConfig, type SiteConfig } from '../api/blog'

const CONFIG_KEY = 'blogConfig'
const WORK_ITEMS_KEY = 'blogWorkItems'

// 默认配置（含工作区子栏默认值）
export const defaultBlogConfig: SiteConfig = {
  siteName: 'My Blog',
  slogan: '记录与分享',
  copyright: '© 2026 My Blog',
  background: '',
  navMenus: [
    { label: '首页', path: '/' },
    { label: '关于', path: '/about' }
  ],
  // 首页每页文章条数：后台可配置（SiteSettings 输入框），默认 12
  articlePageSize: 12,
  workItems: [
    { label: 'item one', index: '4-1', path: '/aichat' },
    { label: 'item two', index: '4-2', path: '' },
    { label: 'item three', index: '4-3', path: '' }
  ]
}

/**
 * 合法化每页文章条数：须为 1~50 的整数，否则回退默认值（防脏数据/越界值）
 * @param v 待校验的条数值（可能是旧配置缺省、字符串或越界数）
 * @returns 1~50 内的合法整数，非法回退 12
 */
const normalizePageSize = (v: unknown): number => {
  const n = typeof v === 'number' ? v : Number(v)
  return Number.isInteger(n) && n >= 1 && n <= 50 ? n : 12
}

/**
 * 读取本地缓存配置并合并默认值（缺省字段兜底）
 * @returns 合并后的完整站点配置
 */
export const loadBlogConfig = (): SiteConfig => {
  try {
    const saved = JSON.parse(localStorage.getItem(CONFIG_KEY) || '{}') as Partial<SiteConfig>
    return {
      ...defaultBlogConfig,
      ...saved,
      // 修复：允许删空菜单——用 !== undefined 而非 .length，
      // 否则把导航菜单全删光后会回退成默认「首页 / 关于」
      navMenus: saved.navMenus !== undefined ? saved.navMenus : defaultBlogConfig.navMenus,
      // 每页条数合法性校验：旧配置缺省或脏数据回退默认 12
      articlePageSize: normalizePageSize(saved.articlePageSize)
    }
  } catch {
    return { ...defaultBlogConfig }
  }
}

/**
 * 读取工作区子栏：默认值 / 本地回读（独立 key blogWorkItems，与博客主配置分开）
 * @returns 工作区子栏配置数组
 */
export const loadWorkItems = (): { label: string; index: string; path: string }[] => {
  try {
    const saved = JSON.parse(localStorage.getItem(WORK_ITEMS_KEY) || '')
    return Array.isArray(saved) && saved.length ? saved : defaultBlogConfig.workItems
  } catch {
    return defaultBlogConfig.workItems
  }
}

/**
 * 保存配置到本地缓存（首屏渲染用）；后端为最终事实源，由 saveSiteConfig 另行写入
 * @param config 要保存的站点配置
 */
export const saveBlogConfig = (config: SiteConfig): void => {
  localStorage.setItem(CONFIG_KEY, JSON.stringify(config))
  localStorage.setItem(WORK_ITEMS_KEY, JSON.stringify(config.workItems))
}

// 响应式全局状态：前台/后台共享同一实例；
// initBlogConfig() 拉到后端值后 Object.assign 覆盖，所有读它的组件自动更新
export const blogConfigState = reactive<SiteConfig>({
  ...loadBlogConfig(),
  workItems: loadWorkItems()
})

/**
 * 启动/挂载时调用：拉取后端全局配置覆盖本地缓存，失败静默（保持本地值）
 * @returns 无返回值（异步执行）
 */
export const initBlogConfig = async (): Promise<void> => {
  try {
    const { config } = await getSiteConfig()
    if (!config) return
    Object.assign(blogConfigState, {
      ...config,
      navMenus: Array.isArray(config.navMenus) ? config.navMenus : [],
      workItems: Array.isArray(config.workItems) ? config.workItems : []
    })
    // 同步本地缓存：本设备下次首屏先渲染后端值（即使后端暂时不可达）
    saveBlogConfig(blogConfigState)
  } catch {
    // 后端不可达（如纯静态预览）：沿用本地缓存 / 默认值
  }
}

/**
 * 读取当前工作区子栏（后台导航「⋯」下拉），始终从全局状态读
 * @returns 工作区子栏配置数组
 */
export const getWorkItems = () => blogConfigState.workItems

/**
 * 保存工作区子栏：更新全局状态 + 写本地缓存 + 写后端 site_config（所有设备全局生效）。
 * 供工作区编辑（HomeView 面板 2-3）调用，与 SiteSettings 共用同一持久化链。
 * @param items 编辑后的子栏数组（元素会被拷贝，不引用调用方可变对象）
 */
export const saveWorkItems = async (
  items: { label: string; index: string; path: string }[]
): Promise<void> => {
  const list = items.map((i) => ({ ...i }))
  // 更新全局响应式状态：顶栏「⋯」下拉、其他设备读取立即同步
  blogConfigState.workItems = list
  // 写本地缓存（首屏渲染 + 后端不可达兜底）
  saveBlogConfig(blogConfigState)
  // 写后端（最终事实源）；失败抛异常由调用方提示
  await saveSiteConfig({ ...blogConfigState })
}
