<!-- src/components/ArticleCard.vue 文章卡片：封面 + 标题 + 摘要 + 分类标签 + 日期，点击跳详情 -->
<template>
  <el-card class="article-card" shadow="hover" @click="goDetail">
    <el-image
      v-if="article.cover"
      :src="article.cover"
      fit="cover"
      class="card-cover"
    />
    <div class="card-header">
      <el-tag size="small">{{ article.category }}</el-tag>
      <span class="card-date">{{ article.date }}</span>
    </div>
    <h3 class="card-title">{{ article.title }}</h3>
    <p class="card-summary">{{ article.summary }}</p>
    <div v-if="article.tags && article.tags.length" class="card-tags">
      <el-tag v-for="t in article.tags" :key="t" size="small" type="info" effect="plain">{{ t }}</el-tag>
    </div>
  </el-card>
</template>

<script lang="ts" setup>
import { useRouter } from 'vue-router'
import type { PropType } from 'vue'
import type { Article } from '../api/blog'

const props = defineProps({
  article: { type: Object as PropType<Article>, required: true }
})

const router = useRouter()

// 点击整卡跳转详情页
const goDetail = () => {
  router.push(`/article/${props.article.id}`)
}
</script>

<style scoped>
.article-card {
  cursor: pointer;
  background: var(--blog-card);
  border-color: var(--blog-border);
}

.card-cover {
  width: 100%;
  height: 150px;
  margin-bottom: 12px;
  border-radius: var(--blog-radius);
}

.card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}

.card-date {
  font-size: 12px;
  color: var(--blog-text-secondary);
}

.card-title {
  margin: 0 0 8px;
  font-size: 16px;
  color: var(--blog-text);
}

.card-summary {
  margin: 0 0 8px;
  font-size: 13px;
  line-height: 1.6;
  color: var(--blog-text-secondary);
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}

.card-tags {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}
</style>
