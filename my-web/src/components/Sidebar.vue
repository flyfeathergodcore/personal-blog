<!-- src/components/Sidebar.vue 侧边栏组件：菜单内容由外部通过 props 传入 -->
<template>
  <el-menu
    :default-active="activeIndex"
    :default-openeds="defaultOpeneds"
    class="sidebar-menu"
    :collapse="collapse"
    :style="menuStyle"
    @select="handleSelect"
  >
    <!-- 遍历外部传入的菜单项；带 children 的渲染为可展开子菜单 -->
    <template v-for="item in menuItems" :key="item.index">
      <el-sub-menu v-if="item.children && item.children.length" :index="item.index">
        <template #title>
          <el-icon v-if="item.icon"><component :is="item.icon" /></el-icon>
          <span>{{ item.label }}</span>
        </template>
        <el-menu-item
          v-for="child in item.children"
          :key="child.index"
          :index="child.index"
        >
          <el-icon v-if="child.icon"><component :is="child.icon" /></el-icon>
          <span>{{ child.label }}</span>
        </el-menu-item>
      </el-sub-menu>

      <!-- 普通菜单项 -->
      <el-menu-item v-else :index="item.index">
        <el-icon v-if="item.icon"><component :is="item.icon" /></el-icon>
        <span>{{ item.label }}</span>
      </el-menu-item>
    </template>
  </el-menu>
</template>

<script lang="ts" setup>
import { ref, watch, computed, type PropType, type Component } from 'vue'
import { useTheme } from '../composables/useTheme'

// 侧边栏菜单项数据结构：label 显示名，index 唯一标识
// icon 为 Element Plus 图标组件（可选），children 为子菜单（可选）
interface SidebarItem {
  label: string
  index: string
  icon?: Component
  children?: SidebarItem[]
}

// 组件属性：菜单内容（menuItems）必须由外部传入
const props = defineProps({
  menuItems: {
    type: Array as PropType<SidebarItem[]>,
    required: true
  },
  // 当前选中菜单项
  defaultActive: {
    type: String,
    default: ''
  },
  // 默认展开的子菜单 index 列表
  defaultOpeneds: {
    type: Array as PropType<string[]>,
    default: () => []
  },
  // 是否折叠（收起为图标条）
  collapse: {
    type: Boolean,
    default: false
  },
  // 侧边栏配色：留空则跟随日夜主题的 CSS 变量（--el-menu-*）
  menuBackground: {
    type: String,
    default: ''
  },
  menuTextColor: {
    type: String,
    default: ''
  },
  activeTextColor: {
    type: String,
    default: '#409EFF'
  }
})

// 组件事件：菜单选中时通知父组件
const emit = defineEmits(['menu-select'])

// 内部状态：当前选中项（同步外部传入的 defaultActive）
const activeIndex = ref(props.defaultActive)
/**
 * 监听外部 defaultActive 变化：同步内部高亮选中项
 */
watch(
  () => props.defaultActive,
  (val) => {
    activeIndex.value = val
  }
)

/**
 * 菜单选中：更新内部高亮并通知父组件
 * @param key 选中菜单项的 index
 * @param keyPath 选中项的完整 index 路径
 */
const handleSelect = (key: string, keyPath: string[]) => {
  activeIndex.value = key
  emit('menu-select', { key, keyPath })
}

const { isDark } = useTheme()
/**
 * 侧边栏配色：暗色模式不覆盖 CSS 变量（强制跟随暗色主题），亮色模式才应用外部自定义颜色
 */
const menuStyle = computed(() => {
  const style: Record<string, string> = {}
  if (isDark.value) return style
  if (props.menuBackground) style['--el-menu-bg-color'] = props.menuBackground
  if (props.menuTextColor) style['--el-menu-text-color'] = props.menuTextColor
  if (props.activeTextColor) style['--el-menu-active-color'] = props.activeTextColor
  return style
})

/**
 * 暴露方法：外部可调用以重置菜单选中状态
 */
defineExpose({
  resetMenu: () => {
    activeIndex.value = ''
  }
})
</script>

<style scoped>
/* 侧边栏容器：占满父容器高度，右侧分割线 */
.sidebar-menu {
  height: 100%;
  border-right: 1px solid var(--el-border-color);
}
</style>
