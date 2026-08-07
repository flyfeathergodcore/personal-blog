<!-- src/components/MenuComponent.vue (增强版) -->
<template>
  <!-- 禁用菜单折叠省略：避免工作区「⋯」按钮被 el-menu 自带的省略号按钮收走，出现双层三个点 -->
  <el-menu
    :default-active="activeIndex"
    class="el-menu-demo custom-menu"
    mode="horizontal"
    :ellipsis="false"
    @select="handleSelect"
    :style="menuStyle"
  >
    <el-menu-item index="1" class="menu-item-switch">
      <!-- 日夜切换：与前台主页看齐（Moon/Sunny 图标 + 全局 html.dark 主题） -->
      <el-switch
        :model-value="isDark"
        size="large"
        inline-prompt
        :active-icon="Moon"
        :inactive-icon="Sunny"
        @change="toggleTheme"
      />
    </el-menu-item>
    <el-menu-item index="2">
      <el-icon><HomeFilled /></el-icon>
      <span>主页</span>
    </el-menu-item>
    <el-menu-item index="3">
      <el-icon><Setting /></el-icon>
      <span>后台</span>
    </el-menu-item>
    <!-- 工作区：下拉式按钮，点击弹出子栏列表（参照官方 el-dropdown 示例） -->
    <el-dropdown trigger="click" @command="handleSubMenuCommand">
      <span
        class="el-dropdown-link"
        :class="{ 'is-active': isWorkItemActive }"
        title="工作区"
      >
        <el-icon><MoreFilled /></el-icon>
        <el-icon class="el-icon--right"><arrow-down /></el-icon>
      </span>
      <template #dropdown>
        <el-dropdown-menu>
          <el-dropdown-item
            v-for="item in workItems"
            :key="item.index"
            :command="item.index"
          >
            <el-icon><Document /></el-icon>
            {{ item.label }}
          </el-dropdown-item>
        </el-dropdown-menu>
      </template>
    </el-dropdown>
    <!-- 退出登录：由 HomeView 的 menu-select 处理清 token 跳登录页 -->
    <el-menu-item index="4">
      <el-icon><SwitchButton /></el-icon>
      <span>退出</span>
    </el-menu-item>
  </el-menu>
</template>

<script lang="ts" setup>
import { ref, computed, type PropType } from 'vue'
import {
  HomeFilled,
  Setting,
  Document,
  MoreFilled,
  ArrowDown,
  SwitchButton,
  Moon,
  Sunny
} from '@element-plus/icons-vue'
import { useTheme } from '../composables/useTheme'

// 工作区子栏数据结构
interface WorkItem {
  label: string
  index: string
}

// 定义组件属性
const props = defineProps({
  defaultActive: {
    type: String,
    default: '1'
  },
  // 导航栏配色：留空则跟随日夜主题的 CSS 变量（--el-menu-*）
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
  },
  // 工作区子栏数据（由父组件传入，支持动态增删）
  workItems: {
    type: Array as PropType<WorkItem[]>,
    default: () => [
      { label: 'item one', index: '4-1' },
      { label: 'item two', index: '4-2' },
      { label: 'item three', index: '4-3' }
    ]
  }
})

// 定义组件事件
const emit = defineEmits(['menu-select'])

// 内部状态
const activeIndex = ref(props.defaultActive)

// 日夜切换：与前台主页看齐（全局 html.dark 主题 + localStorage 持久化）
const { isDark, toggleTheme } = useTheme()

/**
 * 菜单选中：转发选中事件给父组件（含 index 与完整路径）
 * @param key 选中菜单项的 index
 * @param keyPath 选中项的完整 index 路径
 */
const handleSelect = (key: string, keyPath: string[]) => {
  console.log(key, keyPath)
  emit('menu-select', { key, keyPath })
}

/**
 * 当前选中的是否为工作区子栏（用于高亮「⋯」按钮）
 */
const isWorkItemActive = computed(() =>
  props.workItems.some((item) => item.index === activeIndex.value)
)

/**
 * 导航栏配色：暗色模式不覆盖 CSS 变量（强制跟随暗色主题），亮色模式才应用外观自定义颜色
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
 * 工作区子栏点击：模拟菜单选中，同步高亮并通知父组件
 * @param key 点击子栏的 index
 */
const handleSubMenuCommand = (key: string) => {
  activeIndex.value = key
  emit('menu-select', { key, keyPath: [key] })
}

/**
 * 暴露方法给父组件：重置选中态到默认「主页」项
 */
defineExpose({
  resetMenu: () => {
    activeIndex.value = '1'
  }
})
</script>

<style scoped>
.custom-menu {
  display: flex;
  justify-content: flex-end;
  gap: 20px;
  border-bottom: 1px solid var(--el-border-color);
}

/* 让开关菜单项靠左 */
.menu-item-switch {
  margin-right: auto !important;
  display: flex;
  align-items: center;
}

/* 菜单项图标和文字对齐 */
.el-menu-item .el-icon {
  margin-right: 5px;
}

/* 工作区下拉触发器：与菜单项视觉对齐 */
.el-dropdown-link {
  display: flex;
  align-items: center;
  height: 60px;
  padding: 0 20px;
  color: var(--el-menu-text-color, #303133);
  cursor: pointer;
  transition: background-color 0.3s;
}

.el-dropdown-link:hover {
  background-color: var(--el-menu-hover-bg-color, rgba(0, 0, 0, 0.06));
}

/* 选中工作区子栏时高亮触发器 */
.el-dropdown-link.is-active {
  color: var(--el-menu-active-color, #409eff);
}
</style>