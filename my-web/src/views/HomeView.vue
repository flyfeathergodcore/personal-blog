<!-- src/views/HomeView.vue 首页视图 -->
<template>
  <div>
    <MenuComponent
      :default-active="activeMenu"
      :work-items="workItems"
      @menu-select="handleMenuSelect"
      :menu-background="menuBgColor"
      :menu-text-color="menuTextColor"
      active-text-color="#409EFF"
    />

    <!-- 主体区：左侧边栏 + 右侧内容 -->
    <div class="home-body">
      <!-- 侧边栏：菜单内容由外部传入 -->
      <Sidebar
        :menu-items="sidebarItems"
        :default-active="sidebarActive"
        :default-openeds="['2', '3']"
        @menu-select="handleSidebarSelect"
      />

      <div class="content">
        <!-- 状态调试信息（占位内容，后期可移除） -->
        <div class="status-bar">
          主题: {{ isDark ? '🌙 夜晚' : '🌞 白天' }}｜顶栏: {{ activeMenu }}｜侧边栏: {{ sidebarActive }}
        </div>

        <!-- 内容区：按侧边栏选中项动态切换对应面板 -->
        <component :is="activePanel" v-if="activePanel" />
        <el-empty v-else description="请选择菜单" />
      </div>
    </div>

  </div>
</template>

<script lang="ts" setup>
import { ref, computed, type Component } from 'vue'
import { useRouter } from 'vue-router'
import { useTheme } from '../composables/useTheme'
import { loadMenusRaw, toSidebarItems } from '../composables/useSidebarMenus'
import MenuComponent from '../components/NavigationBar.vue'
import Sidebar from '../components/Sidebar.vue'
import Dashboard from '../components/Dashboard.vue'
import ArticleManager from '../components/ArticleManager.vue'
import ResourceManager from '../components/ResourceManager.vue'
import CategoryManager from '../components/CategoryManager.vue'
import ColorSettings from '../components/ColorSettings.vue'
import SiteSettings from '../components/SiteSettings.vue'
import UserManager from '../components/UserManager.vue'
import MenuManager from '../components/MenuManager.vue'
import WorkspaceSettings from '../components/WorkspaceSettings.vue'

const router = useRouter()

const activeMenu = ref('1')
const sidebarActive = ref('1')

// 日夜主题状态：由导航栏开关切换（与前台共用 useTheme，全局 html.dark）
const { isDark } = useTheme()

// 侧边栏菜单数据：后台「菜单管理」可编辑（localStorage.blogSidebarMenus），
// 挂载时读取并映射图标组件；无自定义则用默认菜单
const sidebarItems = ref(toSidebarItems(loadMenusRaw()))

// 面板映射：key 为侧边栏菜单 index，value 为对应组件
const panels: Record<string, Component> = {
  '1': Dashboard, // 仪表盘：站点访问频率（PV/QPS）与负载监控
  '2-1': UserManager,
  '2-2': MenuManager,
  '2-3': WorkspaceSettings,
  '3-1': ArticleManager,
  '3-2': ResourceManager,
  '3-3': CategoryManager,
  '3-4': ColorSettings,
  '3-5': SiteSettings
}
/**
 * 当前侧边栏选中项对应的面板组件（无对应则返回 null 显示空状态）
 */
const activePanel = computed<Component | null>(() => panels[sidebarActive.value] || null)

/**
 * 读取工作区子栏数据：从 localStorage 回读（后台「站点设置」可编辑），无数据时返回默认值
 * @returns 工作区子栏配置数组
 */
const loadWorkItems = (): { label: string; index: string; path: string }[] => {
  const defaults = [
    { label: 'item one', index: '4-1', path: '/aichat' },
    { label: 'item two', index: '4-2', path: '' },
    { label: 'item three', index: '4-3', path: '' }
  ]
  try {
    const saved = localStorage.getItem('blogWorkItems')
    return saved
      ? (JSON.parse(saved) as { label: string; index: string; path: string }[])
      : defaults
  } catch {
    return defaults
  }
}
const workItems = ref(loadWorkItems())

// 导航栏外观：外观设置面板保存的自定义颜色（未自定义则留空，跟随日夜主题的 CSS 变量）
const menuBgColor = ref(localStorage.getItem('blogNavBgColor') || '')
const menuTextColor = ref(localStorage.getItem('blogNavTextColor') || '')

/**
 * 顶栏菜单选择处理：工作区子栏跳转链接，主页/后台/退出特殊处理，其余切换激活菜单
 * @param data 菜单选中项（key 为菜单 index）
 */
const handleMenuSelect = (data: { key: string; keyPath: string[] }) => {
  // 工作区子栏点击：跳转到配置的链接（路由跳转 / 外部链接新窗口）
  const workItem = workItems.value.find((i) => i.index === data.key)
  if (workItem) {
    if (workItem.path) {
      if (workItem.path.startsWith('http')) {
        window.open(workItem.path, '_blank')
      } else {
        router.push(workItem.path)
      }
    }
    return
  }
  // 点击「主页」跳转前台首页
  if (data.key === '2') {
    router.push('/')
    return
  }
  // 点击「后台」跳转到后台首页（当前已处于 /admin，保留兼容）
  if (data.key === '3') {
    router.push('/admin')
    return
  }
  // 点击「退出」：清除登录态并回到登录页（守卫会拦截后台访问）
  if (data.key === '4') {
    localStorage.removeItem('blog_token')
    localStorage.removeItem('blog_username')
    router.push('/login')
    return
  }
  console.log('菜单选择:', data)
  activeMenu.value = data.key
}

/**
 * 接收侧边栏选中通知并更新激活面板
 * @param data 侧边栏选中项（key 为菜单 index）
 */
const handleSidebarSelect = (data: { key: string; keyPath: string[] }) => {
  console.log('侧边栏菜单选择:', data)
  sidebarActive.value = data.key
}
</script>

<style scoped>
/* 主体区：侧边栏 + 内容左右布局 */
.home-body {
  display: flex;
  min-height: calc(100vh - 61px);
}

/* 侧边栏容器：固定宽度，撑满高度 */
.home-body :deep(.sidebar-menu) {
  width: 220px;
  flex-shrink: 0;
}

/* 内容区：占满剩余宽度 */
.content {
  flex: 1;
  padding: 24px;
}

/* 状态调试信息（占位内容） */
.status-bar {
  margin-bottom: 16px;
  padding: 8px 12px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
  background: var(--el-fill-color-light);
  border-radius: 4px;
}
</style>
