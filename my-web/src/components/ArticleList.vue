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
import { blogConfigState } from '../composables/useBlogConfig'
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

// 分页状态：每页条数由后台「站点设置 → 每页文章数」控制（可配置，默认 12）；
// 从全局 blogConfigState 读取，后台保存后热生效（blogConfigState 是响应式对象）
const page = ref(1)
const pageSize = computed(() => {
  const n = blogConfigState.articlePageSize
  // 类型收窄 + 合法范围校验：非法/脏数据回退默认 12
  if (typeof n !== 'number' || !Number.isInteger(n) || n < 1 || n > 50) return 12
  return n
})

// 后台改了每页条数后重置到第 1 页，避免当前页码越界导致列表空白
watch(pageSize, () => {
  page.value = 1
})

/**
 * 拉取文章列表与分类数据；失败时写入 error 用于展示重试入口
 */
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

/**
 * 分类切换：重置分页到第一页并通知父组件
 * @param val 选中的分类名
 */
const handleCategoryChange = (val: string) => {
  page.value = 1
  emit('category-change', val)
}

/**
 * 监听分类变化：切换时重新请求文章列表
 */
watch(activeCategory, fetchData)

/**
 * 搜索过滤：标题或摘要包含关键词的文章（关键词为空时返回全部）
 */
const filteredArticles = computed(() => {
  const kw = props.searchKeyword.trim().toLowerCase()
  if (!kw) return allArticles.value
  return allArticles.value.filter(
    (a) => a.title.toLowerCase().includes(kw) || a.summary.toLowerCase().includes(kw)
  )
})

/**
 * 当前页文章：对过滤结果按 page/pageSize 切片
 */
const pagedArticles = computed(() => {
  const start = (page.value - 1) * pageSize.value
  return filteredArticles.value.slice(start, start + pageSize.value)
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
