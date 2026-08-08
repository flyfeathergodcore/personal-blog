<!-- src/components/BlogHeader.vue 前台顶部导航条：数据驱动菜单（读 blogConfig）+ 后台入口 + 日夜切换，吸顶 -->
<template>
  <header class="blog-header">
    <div class="header-inner">
      <router-link to="/" class="header-logo">{{ title }}</router-link>

      <el-menu
        mode="horizontal"
        :ellipsis="false"
        :default-active="activePath"
        class="header-menu"
        @select="handleSelect"
      >
        <el-menu-item v-for="m in menus" :key="m.label" :index="m.path">{{ m.label }}</el-menu-item>
      </el-menu>

      <div class="header-right">
        <el-switch
          :model-value="isDark"
          size="large"
          inline-prompt
          :active-icon="Moon"
          :inactive-icon="Sunny"
          @change="toggleTheme"
        />
        <el-button link class="admin-btn" @click="goAdmin">后台</el-button>
        <!-- 移动端汉堡按钮：桌面隐藏，≤768px 显示（Chat.vue 模式） -->
        <el-button
          class="mobile-menu-btn"
          size="small"
          circle
          :icon="Menu"
          @click="drawerVisible = true"
        />
      </div>
    </div>
  </header>

  <!-- 移动端菜单抽屉：横向菜单隐藏时承载导航项与后台入口 -->
  <el-drawer
    v-model="drawerVisible"
    direction="rtl"
    size="260px"
    :with-header="false"
    class="mobile-nav-drawer"
  >
    <div class="drawer-nav">
      <div
        v-for="m in menus"
        :key="m.label"
        class="drawer-nav-item"
        :class="{ active: m.path === activePath }"
        @click="handleSelect(m.path)"
      >
        {{ m.label }}
      </div>
      <el-divider />
      <div class="drawer-nav-item drawer-admin" @click="goAdmin">⚙️ 后台管理</div>
    </div>
  </el-drawer>
</template>

<script lang="ts" setup>
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Menu, Moon, Sunny } from '@element-plus/icons-vue'
import { useTheme } from '../composables/useTheme'
import { blogConfigState } from '../composables/useBlogConfig'

const props = defineProps({
  title: { type: String, default: '' }
})

const emit = defineEmits(['menu-select'])

const router = useRouter()
const route = useRoute()
const { isDark, toggleTheme } = useTheme()

// 移动端菜单抽屉可见性（hamburger 按钮 CSS 控制显隐）
const drawerVisible = ref(false)

/**
 * 菜单数据：读全局配置（后端 blog_config → blogConfigState，删除「关于」等即时生效）
 */
const menus = computed(() => blogConfigState.navMenus)
/**
 * 导航栏标题：优先取 props.title，缺省用全局站点名
 */
const title = computed(() => props.title || blogConfigState.siteName)

/**
 * 当前路由路径：用于高亮对应菜单项
 */
const activePath = computed(() => route.path)

/**
 * 菜单点击处理：外部链接新窗口打开，站内路由跳转；移动端抽屉选中后自动关闭
 * @param path 点击的菜单路径
 */
const handleSelect = (path: string) => {
  emit('menu-select', { path })
  drawerVisible.value = false
  if (path.startsWith('http')) {
    window.open(path, '_blank')
  } else {
    router.push(path)
  }
}

/**
 * 后台入口：跳转到后台管理页（移动端抽屉内点击同样关闭抽屉）
 */
const goAdmin = () => {
  drawerVisible.value = false
  router.push('/admin')
}
</script>

<style scoped>
.blog-header {
  position: sticky;
  top: 0;
  z-index: 50;
  background: var(--blog-header-bg);
  backdrop-filter: blur(8px);
  border-bottom: 1px solid var(--blog-border);
}

.header-inner {
  display: flex;
  align-items: center;
  gap: 24px;
  max-width: 1080px;
  margin: 0 auto;
  padding: 0 24px;
}

.header-logo {
  font-size: 18px;
  font-weight: 700;
  color: var(--blog-text);
  text-decoration: none;
  white-space: nowrap;
}

/* 菜单铺满剩余宽度（右对齐） */
.header-menu {
  flex: 1;
  border-bottom: none;
}

.header-menu :deep(.el-menu-item) {
  color: var(--blog-text-secondary);
}

.header-menu :deep(.el-menu-item.is-active) {
  color: var(--blog-primary);
}

.header-right {
  display: flex;
  align-items: center;
  gap: 8px;
}

/* 移动端汉堡按钮：桌面隐藏，≤768px 显示 */
.mobile-menu-btn {
  display: none;
}

/* 抽屉内导航项：大点击区，激活态高亮 */
.drawer-nav {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.drawer-nav-item {
  padding: 12px 8px;
  font-size: 14px;
  color: var(--blog-text);
  border-radius: 4px;
  cursor: pointer;
  transition: background-color 0.2s;
}

.drawer-nav-item:hover {
  background: var(--blog-bg-secondary);
}

.drawer-nav-item.active {
  color: var(--blog-primary);
  background: var(--blog-primary-light);
}

.drawer-admin {
  font-weight: 600;
}

/* ══════ 响应式：≤768px 移动端 ══════ */
@media (max-width: 768px) {
  /* 横向菜单隐藏，导航改由抽屉承载 */
  .header-menu {
    display: none;
  }

  /* 后台入口收敛进抽屉，顶栏只留开关 + 汉堡 */
  .admin-btn {
    display: none;
  }

  .mobile-menu-btn {
    display: inline-flex;
  }

  /* 顶栏压缩：缩小 logo 与间距、外边距，避免溢出 */
  .header-inner {
    gap: 8px;
    padding: 0 12px;
  }

  .header-logo {
    font-size: 16px;
    overflow: hidden;
    text-overflow: ellipsis;
  }
}
</style>
