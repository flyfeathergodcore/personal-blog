<!-- src/page/ArticleDetail.vue 文章详情页：md 渲染（MdViewer/MdPage）+ 右侧大纲 + 同分类文章 -->
<template>
  <div class="article-detail">
    <BlogHeader :title="config.siteName" />
    <div class="detail-body">
      <!-- 左侧：同分类文章（点击跳转同分类其他文章） -->
      <aside v-if="sameCategory.length" class="detail-side detail-side-left">
        <el-card shadow="never" class="side-card">
          <template #header>
            <span class="side-title">📚 同分类文章</span>
          </template>
          <ul class="side-list">
            <li v-for="a in sameCategory" :key="a.id" class="side-item">
              <el-link type="primary" :underline="false" @click="router.push(`/article/${a.id}`)">
                {{ a.title }}
              </el-link>
            </li>
          </ul>
        </el-card>
      </aside>

      <!-- 内容区 -->
      <div class="detail-main">
        <el-skeleton v-if="loading" :rows="8" animated />

        <el-result v-else-if="error" icon="error" title="文章加载失败" :sub-title="error">
          <template #extra>
            <el-button @click="router.push('/')">返回首页</el-button>
            <el-button type="primary" @click="loadArticle(articleId)">重试</el-button>
          </template>
        </el-result>

        <template v-else-if="article">
          <h1 class="article-title">{{ article.title }}</h1>
          <div class="article-meta">
            <el-tag size="small">{{ article.category }}</el-tag>
            <span v-if="article.author">{{ article.author }}</span>
            <span>{{ article.date }}</span>
          </div>

          <!-- md 文件地址走 MdPage；否则 md 文本走 MdViewer -->
          <MdPage v-if="article.fileUrl" :file-url="article.fileUrl" />
          <div v-else class="article-content">
            <MdViewer :content="article.content || ''" @anchors="handleAnchors" />
          </div>

        </template>

        <el-empty v-else description="暂无文章" />
      </div>

      <!-- 右侧：大纲锚点（复用现有组件，内部无锚点时自动不渲染） -->
      <aside v-if="anchors.length" class="detail-side">
        <OutlineAnchor :anchors="anchors" />
      </aside>
    </div>
    <BlogFooter :copyright="config.copyright" />
  </div>
</template>

<script lang="ts" setup>
import { ref, computed, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import MdViewer, { type AnchorItem } from '../components/MdViewer.vue'
import OutlineAnchor from '../components/OutlineAnchor.vue'
import MdPage from './MdPage.vue'
import BlogHeader from '../components/BlogHeader.vue'
import BlogFooter from '../components/BlogFooter.vue'
import { getArticleById, getArticles } from '../api/blog'
import type { Article } from '../api/blog'
import { loadBlogConfig } from '../composables/useBlogConfig'

const route = useRoute()
const router = useRouter()
const config = loadBlogConfig()

// 当前文章 id（来自路由参数）
const articleId = computed(() => String(route.params.id || ''))

const article = ref<Article | null>(null)
const sameCategory = ref<Article[]>([])
const loading = ref(true)
const error = ref('')
const anchors = ref<AnchorItem[]>([])

const handleAnchors = (list: AnchorItem[]) => {
  anchors.value = list
}

// 加载文章 + 同分类列表（过滤当前篇）
const loadArticle = async (id: string) => {
  if (!id) return
  loading.value = true
  error.value = ''
  anchors.value = []
  article.value = null
  sameCategory.value = []
  try {
    const data = await getArticleById(id)
    if (!data) {
      error.value = '文章不存在'
      return
    }
    article.value = data
    // 同分类文章：同分类且非当前篇
    const same = await getArticles({ category: data.category })
    sameCategory.value = same.filter((a) => a.id !== id)
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally {
    loading.value = false
  }
}

// 路由变化时重新加载
watch(articleId, (id) => loadArticle(id), { immediate: true })
</script>

<style scoped>
.article-detail {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}

.detail-body {
  flex: 1;
  display: flex;
  gap: 24px;
  max-width: 1080px;
  width: 100%;
  margin: 0 auto;
  padding: 32px 24px;
  align-items: flex-start;
}

.detail-main {
  flex: 1;
  min-width: 0;
}

.article-title {
  margin: 0 0 12px;
  font-size: 26px;
  color: var(--blog-text);
}

.article-meta {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 24px;
  font-size: 13px;
  color: var(--blog-text-secondary);
}

/* 侧边栏通用：吸顶，内容过长时自行滚动 */
.detail-side {
  width: 220px;
  flex-shrink: 0;
  position: sticky;
  top: 70px;
  max-height: calc(100vh - 100px);
  overflow-y: auto;
}

/* 左侧同分类文章栏：比右侧大纲稍窄，给正文留出空间 */
.detail-side-left {
  width: 180px;
}

/* 同分类文章卡片 */
.side-card {
  border: none;
}

.side-title {
  font-size: 14px;
  font-weight: 600;
}

.side-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.side-item {
  line-height: 1.4;
}

.side-item .el-link {
  font-size: 13px;
}
</style>
