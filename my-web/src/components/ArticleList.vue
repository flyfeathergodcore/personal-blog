<!-- src/components/ArticleList.vue 文章列表：分类筛选 + 卡片列表 + 前端分页，数据自取（预留接口 + mock） -->
<template>
  <div class="article-list">
    <!-- 分类筛选 -->
    <el-radio-group v-model="activeCategory" @change="handleCategoryChange">
      <el-radio-button value="">全部</el-radio-button>
      <el-radio-button v-for="c in categories" :key="c.id" :value="c.name">{{ c.name }}</el-radio-button>
    </el-radio-group>

    <!-- 加载中 -->
    <el-skeleton v-if="loading" :rows="4" animated class="list-skeleton" />

    <!-- 加载失败 -->
    <el-result v-else-if="error" icon="error" title="加载失败" :sub-title="error">
      <template #extra>
        <el-button size="small" @click="fetchData">重试</el-button>
      </template>
    </el-result>

    <template v-else>
      <!-- 卡片网格 -->
      <div v-if="pagedArticles.length" class="list-grid">
        <ArticleCard v-for="a in pagedArticles" :key="a.id" :article="a" />
      </div>
      <el-empty v-else description="暂无文章" />

      <!-- 分页：前端分页（mock 规模小），接口就绪后改为服务端分页 -->
      <el-pagination
        v-if="filteredArticles.length > pageSize"
        v-model:current-page="page"
        :page-size="pageSize"
        layout="prev, pager, next"
        :total="filteredArticles.length"
        class="list-pager"
      />
    </template>
  </div>
</template>

<script lang="ts" setup>
import { ref, computed, onMounted, watch } from 'vue'
import { getArticles, getCategories } from '../api/blog'
import type { Article, Category } from '../api/blog'
import ArticleCard from './ArticleCard.vue'

const props = defineProps({
  // 初始分类（可选）
  initialCategory: { type: String, default: '' },
  // 搜索关键词（父组件 BlogHome 传入，过滤标题/摘要）
  searchKeyword: { type: String, default: '' }
})

const emit = defineEmits(['category-change'])

// 数据状态
const categories = ref<Category[]>([])
const allArticles = ref<Article[]>([])
const loading = ref(true)
const error = ref('')
const activeCategory = ref(props.initialCategory)

// 分页状态
const page = ref(1)
const pageSize = 6

// 拉取列表与分类（预留接口 + mock）
const fetchData = async () => {
  loading.value = true
  error.value = ''
  try {
    const [arts, cats] = await Promise.all([
      getArticles({ category: activeCategory.value || undefined }),
      getCategories()
    ])
    allArticles.value = arts
    categories.value = cats
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e)
  } finally {
    loading.value = false
  }
}

// 分类切换：重置到第一页并通知父组件
const handleCategoryChange = (val: string) => {
  page.value = 1
  emit('category-change', val)
}

// 分类变化时重新请求
watch(activeCategory, fetchData)

// 搜索过滤（标题/摘要包含关键词）
const filteredArticles = computed(() => {
  const kw = props.searchKeyword.trim().toLowerCase()
  if (!kw) return allArticles.value
  return allArticles.value.filter(
    (a) => a.title.toLowerCase().includes(kw) || a.summary.toLowerCase().includes(kw)
  )
})

// 当前页文章
const pagedArticles = computed(() => {
  const start = (page.value - 1) * pageSize
  return filteredArticles.value.slice(start, start + pageSize)
})

onMounted(fetchData)
</script>

<style scoped>
.article-list {
  padding-bottom: 32px;
}

.list-skeleton {
  margin-top: 16px;
}

.list-grid {
  margin-top: 20px;
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
  gap: 20px;
}

.list-pager {
  margin-top: 24px;
  justify-content: center;
}
</style>
