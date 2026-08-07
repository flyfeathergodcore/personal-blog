<!-- src/components/SiteSettings.vue 后台：站点设置（含主页导航栏菜单动态添加） -->
<template>
  <div class="manager-panel">
    <div class="panel-head">
      <h3 class="panel-title">⚙️ 站点设置</h3>
      <!-- 保存按钮置于面板顶部，避免内容过长时被挤到视口外找不到 -->
      <el-button type="primary" size="small" @click="save">保存设置</el-button>
    </div>
    <el-form label-width="100px" size="small" class="site-form">
      <el-form-item label="站点名称">
        <el-input v-model="form.siteName" />
      </el-form-item>
      <el-form-item label="标语">
        <el-input v-model="form.slogan" />
      </el-form-item>
      <el-form-item label="版权信息">
        <el-input v-model="form.copyright" />
      </el-form-item>
      <el-form-item label="主页背景">
        <div class="bg-control">
          <el-input
            v-model="form.background"
            placeholder="图片 URL 或 CSS 背景值，留空默认"
            clearable
            class="bg-input"
          />
          <el-button size="small" @click="openImagePicker">从资源库选择</el-button>
          <el-button size="small" @click="form.background = ''">清除</el-button>
        </div>
      </el-form-item>
    </el-form>

    <!-- 导航菜单管理 -->
    <el-divider content-position="left">主页导航栏菜单</el-divider>
    <div class="menu-add-row">
      <el-input v-model="newMenuLabel" placeholder="菜单名称" size="small" class="menu-add-input" />
      <el-input v-model="newMenuPath" placeholder="路由或外部链接" size="small" class="menu-add-input" />
      <el-button type="primary" size="small" :disabled="!newMenuLabel.trim() || !newMenuPath.trim()" @click="addMenu">
        添加菜单
      </el-button>
    </div>
    <el-table :data="form.navMenus" border size="small">
      <el-table-column prop="label" label="名称" />
      <el-table-column prop="path" label="路径" />
      <el-table-column label="操作" width="80">
        <template #default="{ $index }">
          <el-button size="small" type="danger" text @click="removeMenu($index)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>

    <!-- 工作区设置：顶部导航「⋯」下拉子栏（复用现有组件，保存后前台导航刷新生效） -->
    <el-divider content-position="left">工作区设置</el-divider>
    <WorkspaceSettings v-model="workItems" @update:model-value="handleWorkItemsChange" />

    <!-- 从资源库选择背景图：展示已上传的图片资源（type==='image'），点选写入背景 -->
    <el-dialog v-model="imagePickerVisible" title="从资源库选择背景图" width="640px">
      <div class="bg-picker-grid">
        <div
          v-for="img in imageList"
          :key="img.id"
          class="bg-picker-item"
          :class="{ active: form.background === img.url }"
          @click="pickImage(img)"
        >
          <el-image
            :src="'/api/img/' + img.id"
            fit="cover"
            lazy
            class="bg-picker-img"
          />
          <p class="bg-picker-name">{{ img.name }}</p>
        </div>
        <el-empty
          v-if="!imageList.length"
          description="资源库暂无图片，请先到「资源管理」上传"
        />
      </div>
    </el-dialog>
  </div>
</template>

<script lang="ts" setup>
import { reactive, ref, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import {
  blogConfigState,
  saveBlogConfig,
  initBlogConfig,
  getWorkItems
} from '../composables/useBlogConfig'
import { getResources, saveSiteConfig } from '../api/blog'
import type { Resource, BlogConfig } from '../api/blog'
import WorkspaceSettings from './WorkspaceSettings.vue'

// 本地编辑副本：基于全局配置当前值（后端 → store → 表单），点「保存设置」才提交
const form = reactive<BlogConfig>({
  siteName: blogConfigState.siteName,
  slogan: blogConfigState.slogan,
  copyright: blogConfigState.copyright,
  background: blogConfigState.background,
  navMenus: blogConfigState.navMenus.map((m) => ({ ...m }))
})
// 工作区子栏：从全局 store 读（后端为事实源，非本机 localStorage 独享）
const workItems = ref(getWorkItems().map((w) => ({ ...w })))

// 挂载时拉后端最新全局配置，同步进编辑表单（其他设备改过的配置在此面板可见）
onMounted(async () => {
  await initBlogConfig()
  Object.assign(form, {
    siteName: blogConfigState.siteName,
    slogan: blogConfigState.slogan,
    copyright: blogConfigState.copyright,
    background: blogConfigState.background,
    navMenus: blogConfigState.navMenus.map((m) => ({ ...m }))
  })
  workItems.value = getWorkItems().map((w) => ({ ...w }))
})

// 从资源库选择背景图：弹窗可见性 + 图片资源列表
const imagePickerVisible = ref(false)
const imageList = ref<Resource[]>([])

// 打开资源库选择弹窗：只展示图片资源（type==='image'），视频等非图片不参与选背景
const openImagePicker = async () => {
  try {
    const all = await getResources()
    imageList.value = all.filter((r) => r.type === 'image')
  } catch (e) {
    const err = e instanceof Error ? e : new Error(String(e))
    ElMessage.error('加载资源库失败：' + err.message)
  }
  imagePickerVisible.value = true
}

// 选中图片：背景存文件接口路径（/api/img/:id），前台按需加载。
// 不走完整 data URL——大图 data URL 传输慢，且塞进 localStorage.blogConfig 会超限。
const pickImage = (img: Resource) => {
  form.background = '/api/img/' + img.id
  imagePickerVisible.value = false
  ElMessage.success('已选择背景图，点「保存设置」生效')
}

// 工作区子栏变更：即时同步全局 store + 本地缓存（后端在「保存设置」时一并提交）
const handleWorkItemsChange = (items: { label: string; index: string; path: string }[]) => {
  blogConfigState.workItems = items.map((w) => ({ ...w }))
  localStorage.setItem('blogWorkItems', JSON.stringify(items))
}

// 新增菜单项输入
const newMenuLabel = ref('')
const newMenuPath = ref('')

// 添加菜单：校验空值 + 重复路径
const addMenu = () => {
  const label = newMenuLabel.value.trim()
  const path = newMenuPath.value.trim()
  if (!label || !path) return
  const duplicated = form.navMenus.some((m) => m.path === path)
  if (duplicated) {
    ElMessage.warning('该路径已存在')
    return
  }
  form.navMenus.push({ label, path })
  newMenuLabel.value = ''
  newMenuPath.value = ''
}

// 删除菜单
const removeMenu = (index: number) => {
  form.navMenus.splice(index, 1)
}

// 保存：写本地缓存（本设备即时）+ 更新全局 store（同浏览器立即生效）
// + 写后端 blog_config（所有设备全局生效，删除「关于」等对所有设备可见）
const save = async () => {
  const config = {
    ...form,
    navMenus: form.navMenus.map((m) => ({ ...m })),
    workItems: workItems.value.map((w) => ({ ...w }))
  }
  saveBlogConfig(config)
  Object.assign(blogConfigState, config)
  document.title = form.siteName || 'My Blog'
  try {
    await saveSiteConfig(config)
    ElMessage.success('站点设置已保存（全局生效，所有设备可见）')
  } catch (e) {
    const err = e instanceof Error ? e : new Error(String(e))
    ElMessage.error('全局保存失败（后端不可达？）：' + err.message)
  }
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
  margin: 0 0 12px;
  font-size: 14px;
  font-weight: 600;
}

.site-form {
  max-width: 480px;
}

.menu-add-row {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
}

.menu-add-input {
  max-width: 200px;
}

/* 主页背景：输入框 + 按钮一行排布 */
.bg-control {
  display: flex;
  gap: 8px;
  width: 100%;
}

.bg-input {
  flex: 1;
}

/* 从资源库选背景：缩略图网格 */
.bg-picker-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 12px;
}

.bg-picker-item {
  border: 2px solid var(--el-border-color);
  border-radius: 4px;
  overflow: hidden;
  cursor: pointer;
  transition: border-color 0.2s;
}

.bg-picker-item:hover {
  border-color: #409eff;
}

/* 当前已选背景图高亮 */
.bg-picker-item.active {
  border-color: #67c23a;
  box-shadow: 0 0 0 1px #67c23a;
}

.bg-picker-img {
  width: 100%;
  height: 110px;
  display: block;
}

.bg-picker-name {
  margin: 4px 8px;
  font-size: 12px;
  color: var(--el-text-color-regular);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* 面板头部：标题 + 保存按钮同行 */
.panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
</style>
