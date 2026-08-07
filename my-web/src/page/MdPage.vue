<!-- src/page/MdPage.vue Markdown 展示页面：接收后台下发的文件地址，拉取内容解析展示，右侧提供大纲锚点 -->
<template>
  <div class="md-page">
    <!-- 文档区 -->
    <div class="md-body">
      <!-- 加载中：骨架屏占位 -->
      <el-skeleton v-if="loading" :rows="8" animated />

      <!-- 加载失败 -->
      <el-result v-else-if="loadError" icon="error" title="文档加载失败" :sub-title="loadError">
        <template #extra>
          <el-button size="small" @click="loadContent(fileUrl)">重试</el-button>
        </template>
      </el-result>

      <!-- 正常展示：标题 + 解析渲染（MdViewer 解析后通过 anchors 事件回传大纲） -->
      <template v-else-if="mdContent">
        <!-- data URL 且外部未传 fileName 时 docName 为空，不渲染标题，避免把 base64 串当标题 -->
        <h2 v-if="docName" class="doc-title">{{ docName }}</h2>
        <MdViewer :content="mdContent" @anchors="handleAnchors" />
      </template>

      <!-- 无内容 -->
      <el-empty v-else description="暂无文档" />
    </div>

    <!-- 右侧大纲锚点：吸顶显示，点击快速定位标题位置 -->
    <OutlineAnchor class="md-anchor" :anchors="anchors" />
  </div>
</template>

<script lang="ts" setup>
import { ref, watch } from 'vue'
import MdViewer, { type AnchorItem } from '../components/MdViewer.vue'
import OutlineAnchor from '../components/OutlineAnchor.vue'

// props：fileUrl 为后台下发的 md 文件地址（相对路径、完整 URL 或 data URL）；
// fileName 为可选的外部标题（如文章标题）。data URL 里没有路径可推断文件名，
// 若外部不传 fileName 则 docName 留空，标题不渲染（避免把 base64 内容当标题显示）。
const props = defineProps({
  fileUrl: {
    type: String,
    default: ''
  },
  fileName: {
    type: String,
    default: ''
  }
})

// 文档状态
const mdContent = ref('')
const docName = ref('')
const loading = ref(false)
const loadError = ref('')

// 大纲锚点数据（由 MdViewer 解析后回传，供右侧锚点组件渲染）
const anchors = ref<AnchorItem[]>([])
const handleAnchors = (list: AnchorItem[]) => {
  anchors.value = list
}

// 拉取 md 文件内容并展示
// TODO: 接后端后 fileUrl 由后台接口下发（如后台上传 md 后返回文件地址）
const loadContent = async (url: string) => {
  if (!url) {
    mdContent.value = ''
    docName.value = ''
    anchors.value = []
    return
  }
  loading.value = true
  loadError.value = ''
  try {
    const res = await fetch(url)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
    mdContent.value = await res.text()
    // 标题：优先取外部传入的 fileName；普通 URL 再按路径最后一段兜底；
    // data URL 无法推断文件名（之前会把整条 data URL 当标题显示 base64 乱码），
    // 未传 fileName 时 docName 留空，模板 v-if 不渲染标题
    docName.value = props.fileName
    if (!docName.value && !url.startsWith('data:')) {
      docName.value = url.split('/').pop() || url
    }
  } catch (e) {
    mdContent.value = ''
    loadError.value = e instanceof Error ? e.message : String(e)
  } finally {
    loading.value = false
  }
}

// fileUrl 变化时重新拉取；immediate 让挂载时立即加载
watch(() => props.fileUrl, loadContent, { immediate: true })
</script>

<style scoped>
/* 页面布局：文档区 + 右侧锚点列 */
.md-page {
  display: flex;
  gap: 24px;
  align-items: flex-start;
}

/* 文档区：占满剩余宽度 */
.md-body {
  flex: 1;
  min-width: 0;
}

/* 文档标题 */
.doc-title {
  margin: 0 0 16px;
  font-size: 20px;
  color: var(--el-text-color-primary);
  padding-bottom: 10px;
  border-bottom: 1px solid var(--el-border-color);
}

/* 锚点列：右侧吸顶，超高时内部滚动 */
.md-anchor {
  width: 200px;
  flex-shrink: 0;
  position: sticky;
  top: 70px;
  max-height: calc(100vh - 100px);
  overflow-y: auto;
  padding-left: 8px;
  border-left: 1px solid #f0f0f0;
}
</style>
