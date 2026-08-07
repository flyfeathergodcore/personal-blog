<!-- src/components/MdUploader.vue Markdown 上传面板：后台上传 .md 文件，内容通过 change 事件通知父组件 -->
<template>
  <div class="md-uploader-panel">
    <h3 class="panel-title">📄 文档上传</h3>

    <!-- 拖拽上传 .md 文件 -->
    <el-upload
      drag
      accept=".md,.markdown,.mdx"
      :show-file-list="false"
      :http-request="handleUpload"
    >
      <el-icon class="el-icon--upload"><UploadFilled /></el-icon>
      <div class="el-upload__text">
        拖拽 Markdown 文件到此处，或 <em>点击上传</em>
      </div>
      <template #tip>
        <div class="el-upload__tip">支持 .md / .markdown / .mdx；文件上传到后端并持久化</div>
      </template>
    </el-upload>

    <!-- 当前生效文档信息 -->
    <div v-if="fileName" class="current-doc">
      <span>当前文档：{{ fileName }}</span>
      <el-button size="small" text type="danger" @click="handleClear">清除</el-button>
    </div>
  </div>
</template>

<script lang="ts" setup>
import { ref } from 'vue'
import { ElMessage, type UploadRequestOptions } from 'element-plus'
import { uploadMarkdown } from '../api/blog'

// 组件事件：md 上传成功后通知父组件（内容 + 后端返回的 data URL 地址）
const emit = defineEmits<{
  (e: 'change', payload: { fileName: string; content: string; fileUrl: string }): void
}>()

// 当前文档信息（内部自管，父组件被动接收）
const fileName = ref('')
const mdContent = ref('')
const fileUrl = ref('')

// 真实上传：先调后端 /api/upload/md 持久化（multipart → data URL 入库），
// 成功后再本地读一遍文本填入表单，方便直接编辑/保存内容
const handleUpload = async (options: UploadRequestOptions) => {
  try {
    const res = await uploadMarkdown(options.file)
    fileUrl.value = res.url
  } catch (e) {
    ElMessage.error(e instanceof Error ? e.message : '上传失败')
    // onError 期望 el-upload 内部的 UploadAjaxError；业务异常缺 status/method/url 字段，仅作类型断言
    options.onError(e as unknown as Parameters<typeof options.onError>[0])
    return
  }
  const reader = new FileReader()
  reader.onload = () => {
    mdContent.value = String(reader.result || '')
    fileName.value = options.file.name
    emit('change', { fileName: fileName.value, content: mdContent.value, fileUrl: fileUrl.value })
    ElMessage.success(`已上传到后台：${fileName.value}`)
    options.onSuccess({})
  }
  reader.readAsText(options.file)
}

// 清除当前文档，恢复空内容
const handleClear = () => {
  fileName.value = ''
  mdContent.value = ''
  fileUrl.value = ''
  emit('change', { fileName: '', content: '', fileUrl: '' })
}
</script>

<style scoped>
/* 上传面板 */
.md-uploader-panel {
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

/* 当前文档信息行 */
.current-doc {
  margin-top: 12px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 13px;
  color: var(--el-text-color-regular);
}
</style>
