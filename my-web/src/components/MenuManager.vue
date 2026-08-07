<!-- src/components/MenuManager.vue 后台：菜单管理（侧边栏菜单两级编辑，localStorage 持久化） -->
<template>
  <div class="manager-panel">
    <h3 class="panel-title">🗂️ 菜单管理</h3>
    <p class="panel-desc">管理后台侧边栏菜单（分组 + 子项），保存后刷新后台生效。</p>

    <div class="toolbar">
      <el-button size="small" type="primary" @click="addGroup">+ 添加分组</el-button>
    </div>

    <!-- 分组列表：每组 = 分组行 + 子项列表 -->
    <div v-for="(group, gi) in menus" :key="gi" class="menu-group">
      <div class="group-row">
        <el-input v-model="group.label" size="small" placeholder="分组名" class="g-label" />
        <el-input v-model="group.index" size="small" placeholder="index（如 2）" class="g-index" />
        <el-select v-model="group.icon" size="small" class="g-icon">
          <el-option v-for="name in ICON_OPTIONS" :key="name" :label="name" :value="name" />
        </el-select>
        <el-button size="small" type="primary" plain @click="addChild(gi)">+ 添加子项</el-button>
        <el-button size="small" type="danger" text @click="removeGroup(gi)">删除分组</el-button>
      </div>

      <div v-for="(child, ci) in group.children" :key="ci" class="child-row">
        <el-input v-model="child.label" size="small" placeholder="子项名" class="c-label" />
        <el-input v-model="child.index" size="small" placeholder="index（如 2-1）" class="c-index" />
        <el-select v-model="child.icon" size="small" class="c-icon">
          <el-option v-for="name in ICON_OPTIONS" :key="name" :label="name" :value="name" />
        </el-select>
        <el-button size="small" type="danger" text @click="removeChild(gi, ci)">删除</el-button>
      </div>
    </div>

    <div v-if="!menus.length" class="empty-tip">暂无分组，点上方「+ 添加分组」创建。</div>

    <div class="save-row">
      <el-button type="primary" size="small" @click="save">保存菜单</el-button>
      <el-button size="small" @click="reset">重置为默认</el-button>
    </div>
  </div>
</template>

<script lang="ts" setup>
import { ref } from 'vue'
import { ElMessage } from 'element-plus'
import {
  loadMenusRaw,
  saveMenus,
  DEFAULT_SIDEBAR_MENUS,
  ICON_OPTIONS,
  type MenuNode
} from '../composables/useSidebarMenus'

// 本地编辑副本（深拷贝自存储/默认值）
const menus = ref<MenuNode[]>(loadMenusRaw())

/**
 * 添加一个分组（含默认图标与空子项）
 */
const addGroup = () => {
  menus.value.push({ label: '新分组', index: '', icon: 'Setting', children: [] })
}

/**
 * 删除指定下标的分组
 * @param gi 分组下标
 */
const removeGroup = (gi: number) => {
  menus.value.splice(gi, 1)
}

/**
 * 在指定分组下添加一个子项
 * @param gi 分组下标
 */
const addChild = (gi: number) => {
  const group = menus.value[gi]
  if (!group) return
  group.children.push({ label: '新子项', index: '', icon: 'Document', children: [] })
}

/**
 * 删除指定分组下的指定子项
 * @param gi 分组下标
 * @param ci 子项下标
 */
const removeChild = (gi: number, ci: number) => {
  const group = menus.value[gi]
  if (!group) return
  group.children.splice(ci, 1)
}

/**
 * 校验所有分组与子项的 index 是否非空（侧边栏点击靠 index 匹配面板）
 * @returns 校验失败时的错误提示；全部通过返回 null
 */
const validateIndexes = (): string | null => {
  for (const g of menus.value) {
    if (!g.index.trim()) return `分组「${g.label || '(未命名)'}」缺少 index`
    for (const c of g.children) {
      if (!c.index.trim()) return `子项「${c.label || '(未命名)'}」缺少 index`
    }
  }
  return null
}

/**
 * 保存菜单：先校验 index，通过后写入 localStorage（后台刷新后侧边栏生效）
 */
const save = () => {
  const err = validateIndexes()
  if (err) {
    ElMessage.warning(err)
    return
  }
  saveMenus(menus.value)
  ElMessage.success('菜单已保存，刷新后台生效')
}

/**
 * 重置为默认菜单（需再点「保存菜单」才写入）
 */
const reset = () => {
  menus.value = JSON.parse(JSON.stringify(DEFAULT_SIDEBAR_MENUS)) as MenuNode[]
  ElMessage.success('已恢复默认菜单，点「保存菜单」生效')
}
</script>

<style scoped>
.manager-panel {
  margin-top: 16px;
  padding: 16px;
  border: 1px solid var(--el-border-color);
  border-radius: 4px;
}

.panel-title {
  margin: 0 0 4px;
  font-size: 14px;
  font-weight: 600;
}

.panel-desc {
  margin: 0 0 12px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}

.toolbar {
  margin-bottom: 12px;
}

/* 分组卡片 */
.menu-group {
  border: 1px solid var(--el-border-color-light);
  border-radius: 4px;
  padding: 12px;
  margin-bottom: 12px;
  background: var(--el-fill-color-light);
}

/* 分组行：名称 / index / 图标 / 操作 */
.group-row,
.child-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.group-row {
  margin-bottom: 8px;
}

.child-row {
  margin-bottom: 6px;
}

/* 分组输入宽度 */
.g-label {
  flex: 1;
  max-width: 180px;
}
.g-index {
  width: 110px;
}
.g-icon {
  width: 140px;
}

/* 子项输入宽度 */
.c-label {
  flex: 1;
  max-width: 180px;
  margin-left: 24px;
}
.c-index {
  width: 110px;
}
.c-icon {
  width: 140px;
}

.empty-tip {
  padding: 24px;
  text-align: center;
  color: var(--el-text-color-secondary);
  border: 1px dashed var(--el-border-color);
  border-radius: 4px;
}

.save-row {
  margin-top: 16px;
}
</style>
