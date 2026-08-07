<!-- src/components/ArticleManager.vue 后台：文章管理（CRUD + md 上传复用 MdUploader） -->
<template>
  <div class="manager-panel">
    <h3 class="panel-title">📄 文章管理</h3>

    <div class="toolbar">
      <el-button type="primary" size="small" @click="openDialog()">新增文章</el-button>
      <!-- 按分类管理：选中某分类后只显示该分类文章，清空回全部 -->
      <el-select
        v-model="filterCategory"
        placeholder="按分类筛选"
        size="small"
        clearable
        class="filter-select"
        @change="fetchData"
      >
        <el-option v-for="c in categories" :key="c.id" :label="c.name" :value="c.name" />
      </el-select>
    </div>

    <el-table :data="articles" border size="small">
      <el-table-column prop="id" label="ID" width="60" />
      <el-table-column prop="title" label="标题" min-width="160" />
      <el-table-column prop="category" label="分类" width="90" />
      <el-table-column prop="date" label="日期" width="110" />
      <el-table-column label="操作" width="140">
        <template #default="{ row }">
          <el-button size="small" text @click="openDialog(row)">编辑</el-button>
          <el-button size="small" type="danger" text @click="handleDelete(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>

    <!-- 新增/编辑弹窗 -->
    <el-dialog v-model="dialogVisible" :title="editing ? '编辑文章' : '新增文章'" width="640px">
      <el-form :model="form" label-width="80px" size="small">
        <el-form-item label="标题" required>
          <el-input v-model="form.title" placeholder="文章标题" />
        </el-form-item>
        <el-form-item label="分类" required>
          <!-- 支持输入新分类：不存在时保存会自动创建 -->
          <el-select
            v-model="form.category"
            filterable
            allow-create
            default-first-option
            :reserve-keyword="false"
            placeholder="选择或输入新分类（不存在自动创建）"
            class="form-select"
          >
            <el-option v-for="c in categories" :key="c.id" :label="c.name" :value="c.name" />
          </el-select>
        </el-form-item>
        <el-form-item label="标签">
          <el-select
            v-model="form.tags"
            multiple
            filterable
            allow-create
            default-first-option
            :reserve-keyword="false"
            placeholder="输入后回车添加标签（显示在首页卡片左下角）"
            class="form-select"
          />
        </el-form-item>
        <el-form-item label="摘要">
          <el-input v-model="form.summary" type="textarea" :rows="2" placeholder="卡片摘要" />
        </el-form-item>
        <el-form-item label="日期">
          <el-date-picker v-model="form.date" type="date" value-format="YYYY-MM-DD" />
        </el-form-item>
        <el-form-item label="内容">
          <el-input v-model="form.content" type="textarea" :rows="6" placeholder="Markdown 内容" />
        </el-form-item>
        <!-- md 上传：复用现有 MdUploader，文件持久化到后端，fileUrl 随文章保存 -->
        <el-form-item label="上传">
          <MdUploader @change="handleMdChange" />
          <div v-if="form.fileUrl" class="linked-file">
            已关联文档（保存后详情页将展示该文档；重新上传可替换）
          </div>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button size="small" @click="dialogVisible = false">取消</el-button>
        <el-button size="small" type="primary" :disabled="!form.title.trim()" @click="handleSave">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script lang="ts" setup>
import { ref, reactive, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { getArticles, getCategories, saveArticle, deleteArticle, saveCategory } from '../api/blog'
import type { Article, Category } from '../api/blog'
import MdUploader from './MdUploader.vue'

const articles = ref<Article[]>([])
const categories = ref<Category[]>([])
// 分类筛选：空 = 全部，选中某分类只显示该分类文章
const filterCategory = ref('')

const dialogVisible = ref(false)
const editing = ref<Article | null>(null)
const form = reactive<Article>({ id: '', title: '', summary: '', category: '', date: '', content: '', fileUrl: '', tags: [] })

/**
 * 拉取文章与分类列表：文章带分类筛选参数，同时刷新分类（新增分类后下拉即时出现）
 */
const fetchData = async () => {
  const [arts, cats] = await Promise.all([
    getArticles({ category: filterCategory.value || undefined }),
    getCategories()
  ])
  articles.value = arts
  categories.value = cats
}

/**
 * 打开新增/编辑弹窗：row 有值为编辑态，否则为新增态；老数据无 tags 时兜底为空数组
 * @param row 待编辑的文章；不传则新增
 */
const openDialog = (row?: Article) => {
  editing.value = row || null
  Object.assign(form, row || { id: '', title: '', summary: '', category: '', date: '', content: '', fileUrl: '', tags: [] })
  if (!Array.isArray(form.tags)) form.tags = []
  dialogVisible.value = true
}

/**
 * md 上传回调：把上传内容与 fileUrl（后端 data URL）填入表单，标题为空时用文件名兜底
 * @param payload 上传结果（文件名、内容、后端地址）
 */
const handleMdChange = (payload: { fileName: string; content: string; fileUrl: string }) => {
  form.content = payload.content
  form.fileUrl = payload.fileUrl
  if (!form.title) form.title = payload.fileName.replace(/\.md$/, '')
}

/**
 * 保存文章：新增传空 id（后端过滤为 NULL 走 AUTO_INCREMENT），编辑带原 id；
 * 注意不能传 Date.now() 作 id——13 位时间戳超出 INT 上限会 500
 */
const handleSave = async () => {
  const catName = form.category.trim()
  if (!catName) {
    ElMessage.error('请选择或输入分类')
    return
  }
  try {
    // 输入的是新分类（不在已加载列表中）→ 自动创建，再保存文章
    if (!categories.value.some((c) => c.name === catName)) {
      await saveCategory({ id: '', name: catName })
    }
    const article: Article = { ...form, category: catName, id: editing.value?.id || '' }
    await saveArticle(article)
    ElMessage.success('文章已保存')
    dialogVisible.value = false
    await fetchData()
  } catch (e) {
    ElMessage.error(e instanceof Error ? e.message : '保存失败')
  }
}

/**
 * 删除文章并刷新列表
 * @param row 待删除的文章
 */
const handleDelete = async (row: Article) => {
  await deleteArticle(row.id)
  ElMessage.success('文章已删除')
  await fetchData()
}

onMounted(fetchData)
</script>

<style scoped>
.manager-panel {
  margin-top: 16px;
  padding: 16px;
  border: 1px solid var(--el-border-color);
  border-radius: 4px;
}

.panel-title {
  margin: 0 0 12px;
  font-size: 14px;
  font-weight: 600;
}

.toolbar {
  margin-bottom: 12px;
}

.form-select {
  width: 100%;
}

.filter-select {
  width: 160px;
  margin-left: 12px;
}

.linked-file {
  margin-top: 4px;
  font-size: 12px;
  color: #67c23a;
}
</style>
