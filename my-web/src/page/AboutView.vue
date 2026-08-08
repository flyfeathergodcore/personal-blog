<!-- src/page/AboutView.vue 关于页：站点介绍静态内容 -->
<template>
  <div class="about-page">
    <PageBackground :background="config.background" />
    <BlogHeader :title="config.siteName" />
    <div class="about-main">
      <h1 class="about-title">关于本站</h1>
      <p class="about-text">
        这是一个基于 Vue 3 + Element Plus 构建的博客站。前台负责内容展示，后台提供文章、
        资源、分类、色彩与站点设置等控制能力。
      </p>
      <p class="about-text">本站支持 Markdown 文档渲染、大纲锚点导航、日/夜模式切换，更多能力持续建设中。</p>
    </div>
    <BlogFooter :copyright="config.copyright" />
  </div>
</template>

<script lang="ts" setup>
import BlogHeader from '../components/BlogHeader.vue'
import BlogFooter from '../components/BlogFooter.vue'
import PageBackground from '../components/PageBackground.vue'
import { loadBlogConfig } from '../composables/useBlogConfig'

const config = loadBlogConfig()
</script>

<style scoped>
.about-page {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

.about-main {
  flex: 1;
  max-width: 1080px;
  width: 100%;
  margin: 0 auto;
  padding: 48px 24px;
}

.about-title {
  margin: 0 0 16px;
  font-size: 24px;
  color: var(--blog-text);
}

.about-text {
  font-size: 14px;
  line-height: 1.8;
  color: var(--blog-text-secondary);
}

/* ══════ 响应式：≤768px 移动端 ══════ */
@media (max-width: 768px) {
  .about-main {
    padding: 32px 12px;
  }
}
</style>
