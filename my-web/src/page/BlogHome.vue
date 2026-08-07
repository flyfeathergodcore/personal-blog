<!-- src/page/BlogHome.vue 前台博客首页：背景 + 导航条 + Hero + 文章列表 + 页脚 + AI 占位 -->
<template>
  <div class="blog-home">
    <PageBackground :background="config.background" />
    <BlogHeader :title="config.siteName" />
    <div class="home-main">
      <BlogHero :title="config.siteName" :slogan="config.slogan" @search="handleSearch" />
      <ArticleList :search-keyword="keyword" />
    </div>
    <BlogFooter :copyright="config.copyright" />
    <AiAssistant />
  </div>
</template>

<script lang="ts" setup>
import { ref, onMounted } from 'vue'
import BlogHeader from '../components/BlogHeader.vue'
import BlogHero from '../components/BlogHero.vue'
import BlogFooter from '../components/BlogFooter.vue'
import PageBackground from '../components/PageBackground.vue'
import AiAssistant from '../components/AiAssistant.vue'
import ArticleList from '../components/ArticleList.vue'
import { blogConfigState, initBlogConfig } from '../composables/useBlogConfig'

// 站点配置：响应式全局状态（后台保存 → 后端 blog_config → 所有设备同步）
const config = blogConfigState
// 挂载时拉后端全局配置，覆盖本地缓存（删除「关于」等修改对所有设备生效）
onMounted(initBlogConfig)

// 搜索关键词：Hero 触发后传给 ArticleList 过滤（Task 5 接入）
const keyword = ref('')
const handleSearch = (kw: string) => {
  keyword.value = kw
}
</script>

<style scoped>
.blog-home {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

.home-main {
  flex: 1;
  max-width: 1080px;
  width: 100%;
  margin: 0 auto;
  padding: 0 24px;
}
</style>
