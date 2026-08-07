<!-- src/components/CategoryManager.vue 后台：分类管理（预留接口 + mock） -->
<template>
  <div class="manager-panel">
    <h3 class="panel-title">🏷️ 分类管理</h3>
    <div class="add-row">
      <el-input v-model="newName" placeholder="新分类名称" size="small" class="add-input" />
      <el-button type="primary" size="small" :disabled="!newName.trim()" @click="handleAdd">添加</el-button>
    </div>
    <el-table :data="categories" border size="small">
      <el-table-column prop="id" label="ID" width="80" />
      <el-table-column prop="name" label="分类名" />
      <el-table-column label="操作" width="100">
        <template #default="{ row }">
          <el-button size="small" type="danger" text @click="handleDelete(row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>
  </div>
</template>

<script lang="ts" setup>
import { ref, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { getCategories, saveCategory, deleteCategory } from '../api/blog'
import type { Category } from '../api/blog'

const categories = ref<Category[]>([])
const newName = ref('')

/**
 * 拉取分类列表
 */
const fetchData = async () => {
  categories.value = await getCategories()
}

/**
 * 添加分类：名称非空校验；传空 id 让后端走 INSERT + AUTO_INCREMENT
 */
const handleAdd = async () => {
  const name = newName.value.trim()
  if (!name) return
  try {
    // 新增不传时间戳 id（13 位超 INT 上限，后端会当 UPDATE 处理导致添加无效果）；
    // 传空 id 让后端走 INSERT + AUTO_INCREMENT
    await saveCategory({ id: '', name })
    newName.value = ''
    ElMessage.success('分类已添加')
    await fetchData()
  } catch (e) {
    ElMessage.error(e instanceof Error ? e.message : '添加失败')
  }
}

/**
 * 删除分类并刷新列表
 * @param row 待删除的分类
 */
const handleDelete = async (row: Category) => {
  await deleteCategory(row.id)
  ElMessage.success('分类已删除（mock 演示）')
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

.add-row {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
  max-width: 320px;
}

.add-input {
  flex: 1;
}
</style>
