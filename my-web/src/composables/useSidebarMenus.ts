// 侧边栏菜单读写：后台 MenuManager 写入 localStorage.blogSidebarMenus，
// HomeView 挂载时读取并把图标字符串映射回组件渲染，实现刷新即生效
import type { Component } from 'vue'
import {
  HomeFilled,
  Setting,
  User,
  Menu as MenuIcon,
  DataBoard,
  Grid,
  Document,
  Picture,
  Collection,
  Brush
} from '@element-plus/icons-vue'

// 菜单数据 localStorage 存储键
export const SIDEBAR_MENU_KEY = 'blogSidebarMenus'

// 图标名 → 组件映射（菜单里存字符串名，渲染时映射回组件）
export const ICON_MAP: Record<string, Component> = {
  HomeFilled,
  Setting,
  User,
  Menu: MenuIcon,
  DataBoard,
  Grid,
  Document,
  Picture,
  Collection,
  Brush
}

// 图标可选项（MenuManager 下拉选择）
export const ICON_OPTIONS = Object.keys(ICON_MAP)

// 菜单节点：icon 为图标名字符串（JSON 序列化友好）；只支持两级（分组 → 子项）
export interface MenuNode {
  label: string
  index: string
  icon: string
  children: MenuNode[]
}

// 默认侧边栏菜单（index 与 HomeView 面板 panels 映射约定一致）
export const DEFAULT_SIDEBAR_MENUS: MenuNode[] = [
  { label: '仪表盘', index: '1', icon: 'DataBoard', children: [] },
  {
    label: '系统管理',
    index: '2',
    icon: 'Setting',
    children: [
      { label: '用户管理', index: '2-1', icon: 'User', children: [] },
      { label: '菜单管理', index: '2-2', icon: 'Menu', children: [] },
      { label: '工作区', index: '2-3', icon: 'Grid', children: [] }
    ]
  },
  {
    label: '博客管理',
    index: '3',
    icon: 'Document',
    children: [
      { label: '文章管理', index: '3-1', icon: 'Document', children: [] },
      { label: '资源管理', index: '3-2', icon: 'Picture', children: [] },
      { label: '分类管理', index: '3-3', icon: 'Collection', children: [] },
      { label: '色彩控制', index: '3-4', icon: 'Brush', children: [] },
      { label: '站点设置', index: '3-5', icon: 'Setting', children: [] }
    ]
  }
]

/**
 * 读取原始菜单（字符串 icon 版，MenuManager 编辑用）；无数据返回默认深拷贝
 * @returns 菜单节点数组
 */
export const loadMenusRaw = (): MenuNode[] => {
  try {
    const saved = JSON.parse(localStorage.getItem(SIDEBAR_MENU_KEY) || '') as MenuNode[]
    if (Array.isArray(saved) && saved.length) return saved
  } catch {
    // 解析失败走默认
  }
  return JSON.parse(JSON.stringify(DEFAULT_SIDEBAR_MENUS)) as MenuNode[]
}

/**
 * 保存菜单到 localStorage（MenuManager 调用）
 * @param menus 要保存的菜单节点数组
 */
export const saveMenus = (menus: MenuNode[]): void => {
  localStorage.setItem(SIDEBAR_MENU_KEY, JSON.stringify(menus))
}

// HomeView 用：把字符串 icon 映射为组件，生成 Sidebar 需要的菜单结构
export interface SidebarItemForRender {
  label: string
  index: string
  icon?: Component
  children: { label: string; index: string; icon?: Component }[]
}

/**
 * 把菜单里的字符串 icon 映射为组件，生成 Sidebar 渲染所需的菜单结构
 * @param menus 原始菜单节点（icon 为字符串）
 * @returns 可供 Sidebar 渲染的菜单数组
 */
export const toSidebarItems = (menus: MenuNode[]): SidebarItemForRender[] =>
  menus.map((m) => ({
    label: m.label,
    index: m.index,
    icon: ICON_MAP[m.icon],
    children: m.children.map((c) => ({
      label: c.label,
      index: c.index,
      icon: ICON_MAP[c.icon]
    }))
  }))
