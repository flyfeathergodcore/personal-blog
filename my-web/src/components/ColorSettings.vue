<!-- src/components/ColorSettings.vue 后台：前台色彩控制（写入 CSS 变量 + localStorage，真联动） -->
<template>
  <div class="manager-panel">
    <h3 class="panel-title">🎨 前台色彩控制</h3>
    <el-form
      :label-position="isMobile ? 'top' : 'right'"
      :label-width="isMobile ? 'auto' : '120px'"
      size="small"
      class="color-form"
    >
      <el-form-item v-for="item in colorItems" :key="item.key" :label="item.label">
        <el-color-picker v-model="colors[item.key]" @change="applyColors" />
        <span class="color-code">{{ colors[item.key] }}</span>
      </el-form-item>
    </el-form>

    <!-- 外观设置：导航栏配色 + 登录页背景图（复用现有组件，状态持久化到 localStorage） -->
    <el-divider content-position="left">外观设置（导航栏 / 登录页）</el-divider>
    <Appearance
      :key="appearanceKey"
      :default-bg-color="navBgColor"
      @save="handleAppearanceSave"
    />

    <el-button size="small" @click="resetColors">恢复默认</el-button>
  </div>
</template>

<script lang="ts" setup>
import { reactive, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { applyColorOverrides } from '../composables/useTheme'
import { useViewport } from '../composables/useViewport'
import Appearance from './Appearance.vue'

// 视口断点：移动端表单 label 置顶
const { isMobile } = useViewport()

const COLORS_KEY = 'blogColors'

// 可配置的色彩项：key 对应 CSS 变量名（--blog-{key}）
const colorItems = [
  { key: 'bg', label: '页面背景' },
  { key: 'bg-secondary', label: '次级背景' },
  { key: 'card', label: '卡片背景' },
  { key: 'text', label: '主文字' },
  { key: 'text-secondary', label: '次要文字' },
  { key: 'border', label: '边框' },
  { key: 'primary', label: '主色' }
]

// 默认配色
const defaults: Record<string, string> = {
  bg: '#ffffff',
  'bg-secondary': '#f5f7fa',
  card: '#ffffff',
  text: '#303133',
  'text-secondary': '#606266',
  border: '#e6e6e6',
  primary: '#409eff'
}

const colors = reactive<Record<string, string>>({ ...defaults })

// 初始化：读保存值
try {
  const saved = JSON.parse(localStorage.getItem(COLORS_KEY) || '{}') as Record<string, string>
  for (const key of Object.keys(defaults)) {
    if (saved[key]) colors[key] = saved[key]
  }
} catch {
  // 忽略损坏数据
}

/**
 * 应用色彩覆盖到页面 CSS 变量并持久化到 localStorage
 */
const applyColors = () => {
  const record: Record<string, string> = { ...colors }
  applyColorOverrides(record)
  localStorage.setItem(COLORS_KEY, JSON.stringify(record))
  ElMessage.success('色彩已生效')
}

// ===== 外观设置：导航栏配色 + 登录页背景（持久化，前台/登录页刷新生效） =====

// 导航栏背景色（从 localStorage 回读，作为 Appearance 的初始值）
const navBgColor = ref(localStorage.getItem('blogNavBgColor') || '#f0f2f5')
// 外观重挂载标识：恢复默认时 +1 强制 Appearance 重置为默认值
const appearanceKey = ref(0)

/**
 * 保存外观设置：导航栏配色 + 登录页背景图统一持久化；
 * 只有点保存才写入 localStorage（Appearance 不再自动保存）
 * @param payload 外观配置（导航栏背景/文字色 + 登录页两张背景图）
 */
const handleAppearanceSave = (payload: {
  backgroundColor: string
  textColor: string
  loginBgImage: string
  loginCardBgImage: string
}) => {
  navBgColor.value = payload.backgroundColor
  localStorage.setItem('blogNavBgColor', payload.backgroundColor)
  localStorage.setItem('blogNavTextColor', payload.textColor)
  localStorage.setItem('loginBgImage', payload.loginBgImage)
  localStorage.setItem('loginCardBgImage', payload.loginCardBgImage)
  ElMessage.success('外观设置已保存')
}

/**
 * 恢复默认：清除 CSS 色彩覆盖与外观配置，重挂载 Appearance 恢复默认值
 */
const resetColors = () => {
  Object.assign(colors, defaults)
  localStorage.removeItem(COLORS_KEY)
  const root = document.documentElement
  for (const key of Object.keys(defaults)) {
    root.style.removeProperty(`--blog-${key}`)
  }
  // 重置外观：清除导航栏配色与登录页背景配置，并重挂载 Appearance 恢复默认
  navBgColor.value = '#f0f2f5'
  localStorage.removeItem('blogNavBgColor')
  localStorage.removeItem('blogNavTextColor')
  localStorage.removeItem('loginBgImage')
  localStorage.removeItem('loginCardBgImage')
  appearanceKey.value++
  ElMessage.success('已恢复默认色彩')
}
</script>

<style scoped>
.manager-panel {
  margin-top: 16px;
  padding: 16px;
  border: 1px solid var(--el-border-color);
  border-radius: 4px;
}

.panel-title {
  margin: 0 0 12px;
  font-size: 14px;
  font-weight: 600;
}

.color-form {
  max-width: 480px;
}

.color-code {
  margin-left: 8px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
  font-family: monospace;
}
</style>
