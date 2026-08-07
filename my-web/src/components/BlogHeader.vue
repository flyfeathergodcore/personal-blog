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
        <el-button link @click="goAdmin">后台</el-button>
      </div>
    </div>
  </header>
</template>

<script lang="ts" setup>
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { Moon, Sunny } from '@element-plus/icons-vue'
import { useTheme } from '../composables/useTheme'
import { blogConfigState } from '../composables/useBlogConfig'

const props = defineProps({
  title: { type: String, default: '' }
})

const emit = defineEmits(['menu-select'])

const router = useRouter()
const route = useRoute()
const { isDark, toggleTheme } = useTheme()

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
 * 菜单点击处理：外部链接新窗口打开，站内路由跳转
 * @param path 点击的菜单路径
 */
const handleSelect = (path: string) => {
  emit('menu-select', { path })
  if (path.startsWith('http')) {
    window.open(path, '_blank')
  } else {
    router.push(path)
  }
}

/**
 * 后台入口：跳转到后台管理页
 */
const goAdmin = () => {
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
</style>
