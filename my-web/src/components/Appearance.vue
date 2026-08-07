<!-- src/components/Appearance.vue 外观设置组件 -->
<template>
  <div class="appearance-panel">
    <h3 class="panel-title">🎨 外观设置</h3>

    <!-- 导航栏背景色控制 -->
    <div class="setting-row">
      <span class="setting-label">导航栏背景色</span>
      <div class="bg-control">
        <button
          v-for="preset in bgPresets"
          :key="preset.value"
          class="preset-btn"
          :style="{ backgroundColor: preset.value }"
          :title="preset.name"
          :class="{ active: bgColor === preset.value }"
          @click="bgColor = preset.value"
        />
        <input
          v-model="bgColor"
          type="color"
          class="color-picker"
          title="自定义颜色"
        />
        <span class="color-code">{{ bgColor }}</span>
      </div>
    </div>

    <!-- 文字颜色：根据背景亮度自动适配，只读展示 -->
    <div class="setting-row">
      <span class="setting-label">导航栏文字颜色</span>
      <span class="color-code" :style="{ color: textColor }">{{ textColor }}（自动适配）</span>
    </div>

    <!-- 登录页背景图片 -->
    <div class="setting-row">
      <span class="setting-label">登录页背景图</span>
      <div class="img-control">
        <el-input
          v-model="loginBgImage"
          size="small"
          placeholder="图片 URL，留空使用默认渐变背景"
          clearable
          class="img-url-input"
        />
        <button
          v-for="preset in presetImages"
          :key="preset.url"
          class="img-preset"
          :style="{ backgroundImage: `url(${preset.url})` }"
          :title="preset.name"
          @click="loginBgImage = preset.url"
        />
        <el-button size="small" @click="loginBgImage = ''">清除</el-button>
        <el-button size="small" @click="openUpload('login')">上传</el-button>
        <el-button size="small" @click="openPicker('login')">选资源</el-button>
      </div>
    </div>

    <!-- 登录框背景图片 -->
    <div class="setting-row">
      <span class="setting-label">登录框背景图</span>
      <div class="img-control">
        <el-input
          v-model="loginCardBgImage"
          size="small"
          placeholder="图片 URL，留空使用默认白色背景"
          clearable
          class="img-url-input"
        />
        <button
          v-for="preset in presetImages"
          :key="preset.url"
          class="img-preset"
          :style="{ backgroundImage: `url(${preset.url})` }"
          :title="preset.name"
          @click="loginCardBgImage = preset.url"
        />
        <el-button size="small" @click="loginCardBgImage = ''">清除</el-button>
        <el-button size="small" @click="openUpload('card')">上传</el-button>
        <el-button size="small" @click="openPicker('card')">选资源</el-button>
      </div>
    </div>

    <!-- 上传图片对话框 -->
    <el-dialog v-model="uploadVisible" title="上传背景图" width="420px">
      <el-upload
        drag
        :show-file-list="false"
        :http-request="handleCustomUpload"
        accept="image/*"
      >
        <el-icon class="el-icon--upload"><UploadFilled /></el-icon>
        <div class="el-upload__text">
          拖拽图片到此处，或 <em>点击上传</em>
        </div>
        <template #tip>
          <div class="el-upload__tip">预留上传接口（TODO：接后端后文件持久化到服务器）</div>
        </template>
      </el-upload>
    </el-dialog>

    <!-- 从资源库选择图片对话框（真实资源库：GET /api/resources，仅图片） -->
    <el-dialog v-model="pickerVisible" title="从资源库选择" width="600px">
      <div class="server-images">
        <div
          v-for="img in serverImages"
          :key="img.id"
          class="server-image-item"
          :class="{ active: currentValue === '/api/img/' + img.id }"
          @click="applyImage(img); pickerVisible = false"
        >
          <el-image :src="'/api/img/' + img.id" fit="cover" lazy class="server-image" />
          <p class="server-image-name">{{ img.name }}</p>
        </div>
        <el-empty
          v-if="!serverImages.length"
          description="资源库暂无图片，请先到「资源管理」上传"
        />
      </div>
    </el-dialog>

    <!-- 保存操作区：显式保存全部外观设置（导航栏配色 + 登录页背景图），点保存才持久化 -->
    <div class="appearance-actions">
      <el-button type="primary" size="small" @click="handleSave">保存设置</el-button>
    </div>
  </div>
</template>

<script lang="ts" setup>
import { ref, computed } from 'vue'
import { ElMessage, type UploadRequestOptions } from 'element-plus'
import { getResources } from '../api/blog'
import type { Resource } from '../api/blog'

// 组件属性：初始背景色（后续后台设置时可直接传入已保存的配置值）
const props = defineProps({
  defaultBgColor: {
    type: String,
    default: '#f0f2f5'
  }
})

// 组件事件：点击「保存设置」时把当前全部外观配置交给父组件持久化
const emit = defineEmits<{
  (e: 'save', payload: {
    backgroundColor: string
    textColor: string
    loginBgImage: string
    loginCardBgImage: string
  }): void
}>()

// 内部状态：当前背景色
const bgColor = ref(props.defaultBgColor)

// 预设背景色选项
const bgPresets = [
  { name: '浅灰', value: '#f0f2f5' },
  { name: '白色', value: '#ffffff' },
  { name: '浅蓝', value: '#ecf5ff' },
  { name: '深色', value: '#1f2d3d' }
]

// 登录页背景图片 URL（留空时登录页使用默认渐变/白色背景）。
// 初始值从 localStorage 回读已保存配置，保证重挂载后仍显示历史选择
const loginBgImage = ref(localStorage.getItem('loginBgImage') || '')
const loginCardBgImage = ref(localStorage.getItem('loginCardBgImage') || '')

// 预设图片（picsum 占位图服务，可替换为项目内图片路径）
const presetImages = [
  { name: '山川', url: 'https://picsum.photos/seed/mountain/1920/1080' },
  { name: '海洋', url: 'https://picsum.photos/seed/ocean/1920/1080' },
  { name: '森林', url: 'https://picsum.photos/seed/forest/1920/1080' }
]

// ===== 预留：上传 & 服务器资源选择接口 =====

// 当前正在操作的图片目标：login = 登录页背景，card = 登录框背景
type ImageTarget = 'login' | 'card'
const imageTarget = ref<ImageTarget>('login')

// 上传/资源选择弹窗可见性
const uploadVisible = ref(false)
const pickerVisible = ref(false)

// 服务器资源列表（真实资源库：GET /api/resources，仅图片类型）
const serverImages = ref<Resource[]>([])

// 预留：后端上传接口地址（TODO: 部署后替换为真实接口）
const uploadUrl = '/api/upload'

// 从服务器资源库拉取图片资源（真实接口 GET /api/resources，仅保留图片类型）
const fetchServerImages = async (): Promise<Resource[]> => {
  const all = await getResources()
  return all.filter((r) => r.type === 'image')
}

// 打开上传弹窗，记录当前操作的图片目标
const openUpload = (target: ImageTarget) => {
  imageTarget.value = target
  uploadVisible.value = true
}

// 打开资源选择弹窗并加载资源库图片列表（失败给出提示，不清空已有展示）
const openPicker = async (target: ImageTarget) => {
  imageTarget.value = target
  pickerVisible.value = true
  serverImages.value = []
  try {
    serverImages.value = await fetchServerImages()
  } catch (e) {
    ElMessage.error('加载资源库失败：' + (e instanceof Error ? e.message : String(e)))
  }
}

// 把 url 写入当前目标行（login=登录页背景 / card=登录框背景）
const setTargetValue = (url: string) => {
  if (imageTarget.value === 'login') {
    loginBgImage.value = url
  } else {
    loginCardBgImage.value = url
  }
}

// 从资源库选中：存文件接口短路径（/api/img/:id），前台按需加载。
// 不走完整 data URL——大图 data URL 传输慢，且塞进 localStorage 会超限（与主页背景一致）。
const applyImage = (img: Resource) => {
  setTargetValue('/api/img/' + img.id)
}

// 当前操作目标的背景值（选图弹窗用于高亮已选中的图片）
const currentValue = computed(() =>
  imageTarget.value === 'login' ? loginBgImage.value : loginCardBgImage.value
)

// 预留：自定义上传（TODO: 后期接后端上传接口，如 POST /api/upload）
const handleCustomUpload = (options: UploadRequestOptions) => {
  const formData = new FormData()
  formData.append('file', options.file)
  // TODO: 接后端后调用上传接口，成功后用返回的 URL：
  //   const { data } = await axios.post(uploadUrl, formData)
  //   applyImage(data.url)

  // 临时方案：本地生成预览 URL，让流程先跑通（TODO 接后端后改为返回的持久 URL）
  const previewUrl = URL.createObjectURL(options.file)
  setTargetValue(previewUrl)
  uploadVisible.value = false
  ElMessage.success('上传成功（当前为本地预览，待接入后端接口）')
  options.onSuccess({})
}

// 根据背景色亮度计算文字颜色（ITU-R BT.601 感知亮度公式）
// 深色背景用白色文字，浅色背景用深色文字
const textColor = computed(() => {
  const hex = bgColor.value.replace('#', '')
  const r = parseInt(hex.slice(0, 2), 16)
  const g = parseInt(hex.slice(2, 4), 16)
  const b = parseInt(hex.slice(4, 6), 16)
  const luminance = 0.299 * r + 0.587 * g + 0.114 * b
  return luminance > 150 ? '#333333' : '#ffffff'
})

// 点击「保存设置」：把当前全部外观配置交给父组件持久化。
// 显式保存取代原来的 watch 自动保存，让用户对「已保存」有明确感知
const handleSave = () => {
  emit('save', {
    backgroundColor: bgColor.value,
    textColor: textColor.value,
    loginBgImage: loginBgImage.value,
    loginCardBgImage: loginCardBgImage.value
  })
}
</script>

<style scoped>
/* 外观设置面板 */
.appearance-panel {
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

/* 设置行 */
.setting-row {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 8px;
}

.setting-label {
  width: 100px;
  font-size: 13px;
  color: #333;
}

/* 预设色圆形按钮 */
.preset-btn {
  width: 24px;
  height: 24px;
  border-radius: 50%;
  border: 2px solid var(--el-color-white);
  box-shadow: 0 0 0 1px #ccc;
  cursor: pointer;
  padding: 0;
}

/* 当前选中预设的高亮描边 */
.preset-btn.active {
  box-shadow: 0 0 0 2px #409eff;
}

/* 自定义取色器 */
.color-picker {
  width: 32px;
  height: 24px;
  padding: 0;
  border: none;
  background: transparent;
  cursor: pointer;
}

/* 颜色值展示 */
.color-code {
  font-size: 12px;
  color: #666;
  font-family: monospace;
}

/* 图片设置控件 */
.img-control {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.img-url-input {
  width: 240px;
}

/* 预设图片缩略图按钮 */
.img-preset {
  width: 32px;
  height: 32px;
  border-radius: 4px;
  background-size: cover;
  background-position: center;
  cursor: pointer;
  border: 1px solid var(--el-border-color);
  padding: 0;
}

/* 资源库图片列表：缩略图网格 */
.server-images {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 12px;
}

.server-image-item {
  border-radius: 4px;
  overflow: hidden;
  cursor: pointer;
  border: 1px solid var(--el-border-color);
}

.server-image-item:hover {
  border-color: #409eff;
}

/* 当前已选图片高亮 */
.server-image-item.active {
  border-color: #67c23a;
  box-shadow: 0 0 0 1px #67c23a;
}

.server-image {
  width: 100%;
  height: 110px;
  display: block;
}

.server-image-name {
  margin: 4px 8px;
  font-size: 12px;
  color: var(--el-text-color-regular);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* 保存操作区 */
.appearance-actions {
  margin-top: 12px;
  padding-top: 12px;
  border-top: 1px dashed var(--el-border-color);
}
</style>
