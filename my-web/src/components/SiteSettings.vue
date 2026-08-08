<!-- src/components/SiteSettings.vue 后台：站点设置（含主页导航栏菜单动态添加） -->
<template>
  <div class="manager-panel">
    <div class="panel-head">
      <h3 class="panel-title">⚙️ 站点设置</h3>
      <!-- 保存按钮置于面板顶部，避免内容过长时被挤到视口外找不到 -->
      <el-button type="primary" size="small" @click="save">保存设置</el-button>
    </div>
    <el-form
      :label-position="isMobile ? 'top' : 'right'"
      :label-width="isMobile ? 'auto' : '100px'"
      size="small"
      class="site-form"
    >
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
    <!-- 表格横向滚动容器：窄屏下路径列不溢出 -->
    <div class="table-scroll">
      <el-table :data="form.navMenus" border size="small">
        <el-table-column prop="label" label="名称" min-width="120" />
        <el-table-column prop="path" label="路径" min-width="160" />
        <el-table-column label="操作" width="80">
          <template #default="{ $index }">
            <el-button size="small" type="danger" text @click="removeMenu($index)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </div>

    <!-- 指标与统计：QPS 刷新、落库、清理、仪表盘轮询等参数。
         后端 /api/metrics-config 热生效（存 site_config），重启容器保留 -->
    <el-divider content-position="left">指标与统计</el-divider>
    <el-form
      :label-position="isMobile ? 'top' : 'right'"
      :label-width="isMobile ? 'auto' : '130px'"
      size="small"
      class="metrics-form"
    >
      <el-form-item label="QPS 刷新周期 (ms)">
        <el-input-number v-model="metricsForm.flush_interval_ms" :min="100" :max="1000" :step="100" />
        <span class="metrics-unit">后端 Flush 间隔；越小 QPS 曲线越实时</span>
      </el-form-item>
      <el-form-item label="统计落库周期 (s)">
        <el-input-number v-model="metricsForm.persist_interval_secs" :min="10" :max="3600" :step="10" />
        <span class="metrics-unit">仪表盘历史趋势的落库频率</span>
      </el-form-item>
      <el-form-item label="清理周期 (s)">
        <el-input-number v-model="metricsForm.cleanup_interval_secs" :min="300" :max="86400" :step="300" />
        <span class="metrics-unit">过期统计的检查频率</span>
      </el-form-item>
      <el-form-item label="清理保留 (天)">
        <el-input-number v-model="metricsForm.cleanup_retention_days" :min="7" :max="3650" :step="1" />
        <span class="metrics-unit">site_stats 只保留最近 N 天</span>
      </el-form-item>
      <el-form-item label="实时轮询 (ms)">
        <el-input-number v-model="metricsForm.realtime_refresh_ms" :min="1000" :max="60000" :step="1000" />
        <span class="metrics-unit">仪表盘实时 QPS 刷新间隔</span>
      </el-form-item>
      <el-form-item label="趋势轮询 (ms)">
        <el-input-number v-model="metricsForm.trend_refresh_ms" :min="5000" :max="3600000" :step="60000" />
        <span class="metrics-unit">仪表盘历史趋势刷新间隔</span>
      </el-form-item>
      <el-form-item label="访问者轮询 (ms)">
        <el-input-number v-model="metricsForm.visitor_refresh_ms" :min="1000" :max="60000" :step="1000" />
        <span class="metrics-unit">仪表盘在线 IP 刷新间隔</span>
      </el-form-item>
    </el-form>
    <div class="metrics-actions">
      <el-button type="primary" size="small" @click="saveMetrics">保存指标配置</el-button>
      <span class="metrics-hint">保存后即时生效；重启容器仍保留（存 MySQL），恢复出厂请删 site_config 的 metrics_config 行</span>
    </div>

    <!-- 从资源库选择背景图：展示已上传的图片资源（type==='image'），点选写入背景；移动端全屏 -->
    <el-dialog
      v-model="imagePickerVisible"
      title="从资源库选择背景图"
      :width="isMobile ? '100%' : '640px'"
      :fullscreen="isMobile"
    >
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
import { blogConfigState, saveBlogConfig, initBlogConfig } from '../composables/useBlogConfig'
import {
  metricsConfigState,
  loadMetricsConfig,
  saveMetricsConfigAsync
} from '../composables/useMetricsConfig'
import { useViewport } from '../composables/useViewport'
import { getResources, saveSiteConfig } from '../api/blog'
import type { Resource, BlogConfig, SiteConfig, MetricsConfig } from '../api/blog'

// 本地编辑副本：基于全局配置当前值（后端 → store → 表单），点「保存设置」才提交
const form = reactive<BlogConfig>({
  siteName: blogConfigState.siteName,
  slogan: blogConfigState.slogan,
  copyright: blogConfigState.copyright,
  background: blogConfigState.background,
  navMenus: blogConfigState.navMenus.map((m) => ({ ...m }))
})

// 指标参数编辑副本：基于当前运行时配置（metricsConfigState → 表单），点「保存指标配置」提交
const metricsForm = reactive<MetricsConfig>({ ...metricsConfigState })

// 视口断点：移动端弹窗全屏 + 表单 label 置顶
const { isMobile } = useViewport()

/**
 * 生命周期：挂载时拉取后端最新全局配置与指标参数，并同步进编辑表单
 * （其他设备改过的配置可见；Dashboard 设置的指标值在此回显）
 */
onMounted(async () => {
  await initBlogConfig()
  Object.assign(form, {
    siteName: blogConfigState.siteName,
    slogan: blogConfigState.slogan,
    copyright: blogConfigState.copyright,
    background: blogConfigState.background,
    navMenus: blogConfigState.navMenus.map((m) => ({ ...m }))
  })
  await loadMetricsConfig()
  Object.assign(metricsForm, metricsConfigState)
})

/**
 * 保存指标参数：提交后端（clamp + 热应用 + 落库 site_config），成功后回显 clamp 值
 */
const saveMetrics = async () => {
  try {
    const saved = await saveMetricsConfigAsync({ ...metricsForm })
    Object.assign(metricsForm, saved)
    ElMessage.success('指标配置已保存（即时生效，重启保留）')
  } catch (e) {
    const err = e instanceof Error ? e : new Error(String(e))
    ElMessage.error('指标配置保存失败（后端不可达？）：' + err.message)
  }
}

// 从资源库选择背景图：弹窗可见性 + 图片资源列表
const imagePickerVisible = ref(false)
const imageList = ref<Resource[]>([])

/**
 * 打开资源库选择弹窗：只加载图片资源（type==='image'），视频等非图片不参与选背景
 */
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

/**
 * 选中图片作为背景：背景存文件接口短路径（/api/img/:id）按需加载；
 * 不走完整 data URL——大图传输慢且塞进 localStorage.blogConfig 会超限
 * @param img 选中的图片资源
 */
const pickImage = (img: Resource) => {
  form.background = '/api/img/' + img.id
  imagePickerVisible.value = false
  ElMessage.success('已选择背景图，点「保存设置」生效')
}

// 新增菜单项输入
const newMenuLabel = ref('')
const newMenuPath = ref('')

/**
 * 添加菜单项：校验名称/路径非空与路径不重复，通过后追加并清空输入
 */
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

/**
 * 删除指定下标的菜单项
 * @param index 菜单项下标
 */
const removeMenu = (index: number) => {
  form.navMenus.splice(index, 1)
}

/**
 * 保存设置：写本地缓存（本设备即时）+ 更新全局 store（同浏览器立即生效）
 * + 写后端 blog_config（所有设备全局生效）
 */
const save = async () => {
  // 工作区子栏不再在本面板编辑：沿用全局 store 当前值（保留前端导航「⋯」下拉功能）
  const config: SiteConfig = {
    ...form,
    navMenus: form.navMenus.map((m) => ({ ...m })),
    workItems: blogConfigState.workItems.map((w) => ({ ...w }))
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

/* 指标与统计：数字输入框 + 单位/说明提示 */
.metrics-form {
  max-width: 720px;
}

.metrics-unit {
  margin-left: 10px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}

.metrics-actions {
  margin: 8px 0 4px;
  display: flex;
  align-items: center;
  gap: 10px;
}

.metrics-hint {
  font-size: 12px;
  color: var(--el-text-color-secondary);
}

/* 表格横向滚动：窄屏下路径列不溢出 */
.table-scroll {
  overflow-x: auto;
  -webkit-overflow-scrolling: touch;
}

/* 移动端：菜单添加行允许换行，指标说明换行避免挤压 */
@media (max-width: 768px) {
  .menu-add-row {
    flex-wrap: wrap;
  }

  .menu-add-input {
    max-width: 100%;
    flex: 1 1 100%;
  }

  .metrics-unit,
  .metrics-hint {
    display: block;
    margin-left: 0;
    margin-top: 2px;
  }

  .bg-picker-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}
</style>
