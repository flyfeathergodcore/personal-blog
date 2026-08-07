<!-- src/components/UserManager.vue 后台：用户管理（真实后端 users 表 CRUD） -->
<template>
  <div class="manager-panel">
    <h3 class="panel-title">👤 用户管理</h3>
    <div class="toolbar">
      <el-button type="primary" size="small" @click="openAdd">新增用户</el-button>
    </div>
    <el-table :data="users" border size="small">
      <el-table-column prop="id" label="ID" width="80" />
      <el-table-column prop="username" label="用户名" />
      <el-table-column prop="createdAt" label="创建时间" />
      <el-table-column label="操作" width="150">
        <template #default="{ row }">
          <el-button size="small" text type="primary" @click="openEdit(row)">编辑</el-button>
          <el-button
            size="small"
            text
            type="danger"
            :disabled="row.id === '1'"
            title="种子管理员不可删除"
            @click="handleDelete(row)"
          >
            删除
          </el-button>
        </template>
      </el-table-column>
    </el-table>

    <!-- 新增 / 编辑对话框 -->
    <el-dialog
      v-model="dialogVisible"
      :title="editingId ? '编辑用户' : '新增用户'"
      width="400px"
    >
      <el-form label-width="80px" size="small">
        <el-form-item label="用户名">
          <el-input v-model="form.username" placeholder="登录用户名" />
        </el-form-item>
        <el-form-item :label="editingId ? '新密码' : '密码'">
          <el-input
            v-model="form.password"
            type="password"
            show-password
            :placeholder="editingId ? '留空则不修改密码' : '登录密码'"
          />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button size="small" @click="dialogVisible = false">取消</el-button>
        <el-button
          type="primary"
          size="small"
          :disabled="!form.username.trim() || (!editingId && !form.password)"
          @click="handleSave"
        >
          保存
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script lang="ts" setup>
import { ref, reactive, onMounted } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { getUsers, saveUser, deleteUser } from '../api/blog'
import type { User } from '../api/blog'

const users = ref<User[]>([])
const dialogVisible = ref(false)
const editingId = ref('') // 空 = 新增，非空 = 编辑

const form = reactive({ username: '', password: '' })

const fetchData = async () => {
  users.value = await getUsers()
}

const openAdd = () => {
  editingId.value = ''
  form.username = ''
  form.password = ''
  dialogVisible.value = true
}

const openEdit = (row: User) => {
  editingId.value = row.id
  form.username = row.username
  form.password = '' // 密码不回显，留空表示不修改
  dialogVisible.value = true
}

const handleSave = async () => {
  const username = form.username.trim()
  if (!username) return
  // 新增必须带密码；编辑时密码留空则不携带 password 字段（后端不改密码）
  const payload: User = editingId.value
    ? { id: editingId.value, username }
    : { id: '', username, password: form.password }
  try {
    await saveUser(payload)
    ElMessage.success('用户已保存')
    dialogVisible.value = false
    await fetchData()
  } catch (e) {
    ElMessage.error(e instanceof Error ? e.message : '保存失败')
  }
}

const handleDelete = async (row: User) => {
  try {
    await ElMessageBox.confirm(`确定删除用户「${row.username}」？`, '删除确认', {
      type: 'warning'
    })
  } catch {
    return // 用户取消
  }
  try {
    await deleteUser(row.id)
    ElMessage.success('用户已删除')
    await fetchData()
  } catch (e) {
    ElMessage.error(e instanceof Error ? e.message : '删除失败')
  }
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
</style>
