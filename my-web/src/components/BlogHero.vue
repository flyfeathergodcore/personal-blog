<!-- src/components/BlogHero.vue Hero 标语区：站点名 + slogan + 搜索框 -->
<template>
  <section class="blog-hero">
    <h1 class="hero-title">{{ title }}</h1>
    <p class="hero-slogan">{{ slogan }}</p>
    <el-input
      v-if="showSearch"
      v-model="keyword"
      size="large"
      placeholder="搜索文章…"
      clearable
      class="hero-search"
      @keyup.enter="doSearch"
    >
      <template #prefix>
        <el-icon><Search /></el-icon>
      </template>
    </el-input>
  </section>
</template>

<script lang="ts" setup>
import { ref } from 'vue'
import { Search } from '@element-plus/icons-vue'

defineProps({
  title: { type: String, default: 'My Blog' },
  slogan: { type: String, default: '记录与分享' },
  showSearch: { type: Boolean, default: true }
})

const emit = defineEmits(['search'])

const keyword = ref('')

// 回车触发搜索，把关键词交给父组件（父组件传入 ArticleList 过滤）
const doSearch = () => {
  emit('search', keyword.value)
}
</script>

<style scoped>
.blog-hero {
  padding: 48px 0 32px;
  text-align: center;
}

.hero-title {
  margin: 0 0 8px;
  font-size: 32px;
  font-weight: 700;
  color: var(--blog-text);
}

.hero-slogan {
  margin: 0 0 20px;
  font-size: 15px;
  color: var(--blog-text-secondary);
}

.hero-search {
  max-width: 480px;
}
</style>
