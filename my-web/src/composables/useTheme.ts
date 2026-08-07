// 主题 composable：日/夜切换 + 后台色彩覆盖应用
import { ref } from 'vue'

const THEME_KEY = 'blogTheme'
const COLORS_KEY = 'blogColors'

// 当前是否暗色（localStorage 持久化，默认跟随亮色）
const isDark = ref(localStorage.getItem(THEME_KEY) === 'dark')

/**
 * 应用主题：切换 html.dark class 并持久化选择
 * @param dark 是否为暗色主题
 */
const applyTheme = (dark: boolean) => {
  document.documentElement.classList.toggle('dark', dark)
  localStorage.setItem(THEME_KEY, dark ? 'dark' : 'light')
}

/**
 * 应用后台色彩覆盖：把 { 变量名: 颜色 } 写入 :root 内联样式（优先级最高）
 * @param colors 变量名到颜色值的映射
 */
export const applyColorOverrides = (colors: Record<string, string>) => {
  const root = document.documentElement
  for (const [key, value] of Object.entries(colors)) {
    root.style.setProperty(`--blog-${key}`, value)
  }
}

/**
 * 回读后台保存的色彩覆盖并应用（模块加载时执行一次）
 */
const loadColorOverrides = () => {
  try {
    const saved = JSON.parse(localStorage.getItem(COLORS_KEY) || '{}') as Record<string, string>
    applyColorOverrides(saved)
  } catch {
    // localStorage 数据损坏时忽略
  }
}

/**
 * 切换日/夜主题并持久化
 */
const toggleTheme = () => {
  isDark.value = !isDark.value
  applyTheme(isDark.value)
}

// 初始化：应用持久化主题与色彩覆盖
applyTheme(isDark.value)
loadColorOverrides()

/**
 * 主题 composable：暴露暗色状态与切换方法，供前台/后台全局共用
 * @returns 含 isDark 状态与 toggleTheme 切换方法的对象
 */
export function useTheme() {
  return { isDark, toggleTheme }
}
