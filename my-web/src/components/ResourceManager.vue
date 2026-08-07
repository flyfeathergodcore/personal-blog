<!-- src/components/ResourceManager.vue 后台：资源管理（图片/视频上传、列表、删除、复制地址） -->
<template>
  <div class="manager-panel">
    <h3 class="panel-title">🖼️ 资源管理</h3>

    <!-- 上传：支持图片与视频（后端按 mime 自动区分 type） -->
    <el-upload
      drag
      accept="image/*,video/*"
      :show-file-list="false"
      :disabled="uploading"
      :http-request="handleUpload"
      class="upload-area"
    >
      <el-icon class="el-icon--upload"><UploadFilled /></el-icon>
      <div class="el-upload__text">拖拽图片或视频到此处，或 <em>点击上传</em></div>
    </el-upload>

    <!-- 上传进度条 -->
    <el-progress
      v-if="uploading"
      :percentage="progress"
      :stroke-width="12"
      class="upload-progress"
    />

    <!-- 资源网格：只展示图片与视频（md 文档等非媒体资源不在此管理） -->
    <div v-if="resources.length" class="resource-grid">
      <el-card v-for="r in resources" :key="r.id" class="resource-card" shadow="hover">
        <!-- 视频资源：可播放；其余按图片渲染。src 走 /api/img/:id 按需加载，
             避免列表接口全量下发大文件导致卡死 -->
        <video
          v-if="r.type === 'video'"
          :src="'/api/img/' + r.id"
          controls
          preload="metadata"
          class="resource-video"
        />
        <el-image
          v-else
          :src="'/api/img/' + r.id"
          fit="cover"
          lazy
          class="resource-img"
        />
        <p class="resource-name">{{ r.name }}</p>
        <div class="resource-actions">
          <el-button size="small" @click="copyUrl(r)">复制地址</el-button>
          <el-button size="small" type="danger" @click="handleDelete(r.id)">删除</el-button>
        </div>
      </el-card>
    </div>
    <el-empty v-else description="暂无图片或视频资源" />
  </div>
</template>

<script lang="ts" setup>
import { ref, onMounted } from 'vue'
import { ElMessage, type UploadRequestOptions } from 'element-plus'
import { UploadFilled } from '@element-plus/icons-vue'
import { getResources, uploadImage, deleteResource, getResourceUrl } from '../api/blog'
import type { Resource } from '../api/blog'

const resources = ref<Resource[]>([])
const uploading = ref(false)
const progress = ref(0)

/**
 * 拉取资源列表并过滤出图片/视频（md 文档等非媒体资源不在此管理）
 */
const fetchData = async () => {
  const all = await getResources()
  resources.value = all.filter(
    (r) => r.type === 'image' || r.type === 'video'
  )
}

/**
 * 上传图片/视频：带进度条与明确错误提示（避免后端失败时静默无反馈）
 * @param options el-upload 的请求配置（含文件与回调）
 */
const handleUpload = async (options: UploadRequestOptions) => {
  uploading.value = true
  progress.value = 0
  try {
    await uploadImage(options.file, (p) => {
      progress.value = p
    })
    options.onSuccess({})
    ElMessage.success('上传成功')
    await fetchData()
  } catch (e) {
    const err = e instanceof Error ? e : new Error(String(e))
    // 通知 element-plus 上传失败，避免内部状态挂起
    options.onError?.(err as Parameters<typeof options.onError>[0])
    ElMessage.error('上传失败：' + err.message)
  } finally {
    uploading.value = false
  }
}

/**
 * 复制资源地址到剪贴板：列表不含 url，先按 id 拉取完整地址再复制
 * @param r 资源对象
 */
const copyUrl = async (r: Resource) => {
  const url = await getResourceUrl(r.id)
  if (!url) {
    ElMessage.error('获取资源地址失败')
    return
  }
  await navigator.clipboard.writeText(url)
  ElMessage.success('地址已复制')
}

/**
 * 删除资源并刷新列表
 * @param id 资源 id
 */
const handleDelete = async (id: string) => {
  await deleteResource(id)
  ElMessage.success('资源已删除')
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

.upload-area {
  margin-bottom: 16px;
}

.upload-progress {
  margin-bottom: 16px;
  max-width: 480px;
}

.resource-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 12px;
}

.resource-img {
  width: 100%;
  height: 120px;
}

.resource-video {
  width: 100%;
  height: 120px;
  object-fit: cover;
  background: #000;
}

.resource-name {
  margin: 8px 0;
  font-size: 13px;
  color: var(--el-text-color-regular);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.resource-actions {
  display: flex;
  gap: 8px;
}
</style>
