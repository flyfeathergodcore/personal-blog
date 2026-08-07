<!-- src/components/WorkspaceSettings.vue 工作区编辑组件（后台设置用） -->
<template>
  <div class="workspace-panel">
    <h3 class="panel-title">🗂️ 工作区设置</h3>
    <p class="panel-desc">管理工作区下的子栏目，编辑后点击保存生效；后期后台可直接复用本组件编辑菜单配置。</p>

    <!-- 局域网访问卡片：开关 + 访问地址（附近设备同 WiFi 可访问） -->
    <div class="lan-card">
      <div class="lan-head">
        <div class="lan-title">
          <span class="lan-name">📡 局域网访问</span>
          <el-switch
            v-model="lanEnabled"
            size="small"
            :loading="lanLoading"
            @change="handleLanChange"
          />
        </div>
        <p class="lan-desc">开启后，同一 WiFi 下的设备（手机 / 平板 / 电脑）可通过下方地址访问本网站；关闭后仅本机可访问。</p>
      </div>

      <!-- 开启：显示局域网访问地址，一键复制 -->
      <div v-if="lanEnabled" class="lan-body">
        <div v-if="lanUrl" class="lan-addr">
          <code class="lan-addr-code">{{ lanUrl }}</code>
          <button class="lan-btn" @click="copyLanUrl">复制地址</button>
        </div>
        <p v-else class="lan-tip">正在检测本机局域网 IP…</p>
      </div>
      <!-- 关闭：提示仅本机可访问 -->
      <p v-else class="lan-tip lan-off">局域网访问已关闭，仅本机可通过 {{ localUrl }} 访问</p>
    </div>

    <!-- 子栏列表：label 可编辑，index 自动编号只读展示，path 为点击子栏的跳转链接 -->
    <div class="ws-list">
      <div v-for="item in localItems" :key="item.index" class="ws-row">
        <span class="ws-index">{{ item.index }}</span>
        <el-input
          v-model="item.label"
          size="small"
          placeholder="子栏名称"
          class="ws-label-input"
        />
        <el-input
          v-model="item.path"
          size="small"
          placeholder="跳转链接（路由或外部 URL）"
          class="ws-path-input"
        />
        <button
          class="ws-btn ws-delete-btn"
          :disabled="localItems.length <= 1"
          title="删除该子栏（至少保留一项）"
          @click="removeItem(item.index)"
        >
          删除
        </button>
      </div>
    </div>

    <!-- 添加按钮 + 未保存提示 -->
    <div class="ws-actions">
      <button class="ws-btn ws-add-btn" @click="addItem">+ 添加子栏</button>
      <span v-if="dirty" class="ws-dirty-tip">⚠️ 有未保存的更改</span>
    </div>

    <!-- 底部操作：保存 / 重置 -->
    <div class="ws-footer">
      <button class="ws-btn ws-save-btn" :disabled="!dirty" @click="save">保存</button>
      <button class="ws-btn ws-reset-btn" :disabled="!dirty" @click="reset">重置</button>
    </div>
  </div>
</template>

<script lang="ts" setup>
import { ref, computed, watch, onMounted, type PropType } from 'vue'
import { ElMessage } from 'element-plus'
import { getLanStatus, setLanEnabled } from '../api/blog'

// ===== 局域网访问开关 =====
// 状态来自后端 /api/network/lan（落库 site_config，容器重建后保持）
const lanEnabled = ref(false)
const lanLoading = ref(false)
// 宿主机局域网 IP：build-run.sh 注入 HOST_LAN_IP，后端返回；为空时 WebRTC 兜底
const lanIp = ref('')
/**
 * 访问端口：跟随当前页面（生产 8443，本地 dev 5173）
 */
const port = computed(() => window.location.port || '8443')
/**
 * 局域网访问地址（开启时展示给附近设备）；无 IP 时为空
 */
const lanUrl = computed(() =>
  lanIp.value ? `http://${lanIp.value}:${port.value}` : ''
)
/**
 * 本机访问地址（关闭局域网时提示用）
 */
const localUrl = computed(() => window.location.origin)

/**
 * 切换局域网访问开关：调后端持久化，失败则回滚开关状态
 * @param val 目标开关状态
 */
const handleLanChange = async (val: boolean) => {
  lanLoading.value = true
  try {
    const st = await setLanEnabled(val)
    lanEnabled.value = st.enabled
    // 开启时用 WebRTC 实时探测覆盖后端值（后端是启动时注入，换网后可能过期）
    lanIp.value = st.enabled ? await refreshLanIp(st.lanIp) : st.lanIp
    ElMessage.success(st.enabled ? '已开启局域网访问' : '已关闭局域网访问')
  } catch (e) {
    lanEnabled.value = !val
    ElMessage.error('切换失败：' + (e instanceof Error ? e.message : String(e)))
  } finally {
    lanLoading.value = false
  }
}

/**
 * 复制局域网访问地址到剪贴板；失败时提示手动复制
 */
const copyLanUrl = async () => {
  try {
    await navigator.clipboard.writeText(lanUrl.value)
    ElMessage.success('访问地址已复制，发送给附近设备即可访问')
  } catch {
    ElMessage.info(`请手动复制：${lanUrl.value}`)
  }
}

/**
 * WebRTC 兜底探测宿主机局域网 IP（后端 HOST_LAN_IP 未注入时使用）；
 * 仅安全上下文（https / localhost）可用，取非回环的局域网地址
 * @returns 解析出的局域网 IP，失败或超时返回空字符串
 */
const detectLanIpByWebRTC = (): Promise<string> =>
  new Promise((resolve) => {
    try {
      const pc = new RTCPeerConnection({ iceServers: [] })
      pc.createDataChannel('lan')
      pc.onicecandidate = (e) => {
        const c = e.candidate?.candidate || ''
        const m = /([0-9]{1,3}(\.[0-9]{1,3}){3})/.exec(c)
        if (m && m[1]) {
          const ip = m[1]
          const isLan =
            ip.startsWith('192.168.') ||
            ip.startsWith('10.') ||
            /^172\.(1[6-9]|2\d|3[01])\./.test(ip)
          if (isLan) {
            pc.close()
            resolve(ip)
          }
        }
        if (!e.candidate) {
          pc.close()
          resolve('')
        }
      }
      pc.createOffer()
        .then((o) => pc.setLocalDescription(o))
        .catch(() => resolve(''))
      setTimeout(() => {
        pc.close()
        resolve('')
      }, 3000)
    } catch {
      resolve('')
    }
  })

/**
 * 局域网 IP 实时探测：WebRTC 优先（换网后也能拿到当前值），失败时回落后端注入值
 * @param fallback 后端注入的兜底 IP
 * @returns 探测到的最新局域网 IP
 */
const refreshLanIp = async (fallback: string): Promise<string> => {
  const real = await detectLanIpByWebRTC()
  return real || fallback
}

/**
 * 初始化：读取后端当前开关状态；开启时用 WebRTC 实时探测覆盖后端旧值
 */
const loadLanStatus = async () => {
  try {
    const st = await getLanStatus()
    lanEnabled.value = st.enabled
    lanIp.value = st.enabled ? await refreshLanIp(st.lanIp) : st.lanIp
  } catch {
    // 后端未就绪时保持关闭态，不阻塞工作区面板
  }
}
onMounted(loadLanStatus)

// 工作区子栏数据结构：label 显示名，index 唯一标识，path 为点击子栏时的跳转链接
interface WorkItem {
  label: string
  index: string
  path: string
}

// 受控组件：数据由父组件持有，通过 v-model 双向绑定
const props = defineProps({
  modelValue: {
    type: Array as PropType<WorkItem[]>,
    default: () => []
  }
})

const emit = defineEmits(['update:modelValue'])

// 本地编辑副本：编辑过程不触碰父组件数据，保存时一次性提交
const localItems = ref<WorkItem[]>([])

/**
 * 同步外部 modelValue 到本地编辑副本（深拷贝，避免直接修改 props 内部对象）
 */
watch(
  () => props.modelValue,
  (val) => {
    localItems.value = val.map((item) => ({ ...item }))
  },
  { immediate: true }
)

/**
 * 是否存在未保存的更改：本地副本与外部数据序列化后不一致即为脏
 */
const dirty = computed(
  () => JSON.stringify(localItems.value) !== JSON.stringify(props.modelValue)
)

/**
 * 添加子栏：编号自动递增，跳转链接默认留空（点击时不跳转）
 */
const addItem = () => {
  const next = localItems.value.length + 1
  localItems.value.push({ label: `item ${next}`, index: `4-${next}`, path: '' })
}

/**
 * 删除指定子栏（至少保留一项）
 * @param index 待删除子栏的唯一标识
 */
const removeItem = (index: string) => {
  if (localItems.value.length <= 1) return
  const i = localItems.value.findIndex((item) => item.index === index)
  if (i !== -1) localItems.value.splice(i, 1)
}

/**
 * 保存：把本地编辑副本提交给父组件（v-model 更新）
 */
const save = () => {
  emit('update:modelValue', localItems.value.map((item) => ({ ...item })))
}

/**
 * 重置：放弃本地修改，恢复为父组件传入的数据
 */
const reset = () => {
  localItems.value = props.modelValue.map((item) => ({ ...item }))
}
</script>

<style scoped>
/* 工作区设置面板 */
.workspace-panel {
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

/* 局域网访问卡片 */
.lan-card {
  margin-bottom: 14px;
  padding: 12px 14px;
  border: 1px dashed var(--el-border-color);
  border-radius: 6px;
  background: var(--el-fill-color-light);
}

.lan-head {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.lan-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.lan-name {
  font-size: 13px;
  font-weight: 600;
  color: var(--el-text-color-primary);
}

.lan-desc {
  margin: 0;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}

/* 开启状态：地址展示区 */
.lan-body {
  margin-top: 10px;
}

.lan-addr {
  display: flex;
  align-items: center;
  gap: 10px;
}

.lan-addr-code {
  padding: 6px 10px;
  font-size: 13px;
  color: #409eff;
  background: var(--el-bg-color);
  border: 1px solid var(--el-border-color);
  border-radius: 4px;
  word-break: break-all;
}

.lan-btn {
  padding: 5px 12px;
  font-size: 12px;
  color: #fff;
  background: #409eff;
  border: 1px solid #409eff;
  border-radius: 4px;
  cursor: pointer;
}

.lan-btn:hover {
  background: #66b1ff;
}

/* 提示文字 */
.lan-tip {
  margin: 0;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}

.lan-off {
  margin-top: 10px;
  color: #f56c6c;
}

/* 子栏列表 */
.ws-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.ws-row {
  display: flex;
  align-items: center;
  gap: 10px;
}

/* 只读的编号 */
.ws-index {
  width: 40px;
  font-size: 12px;
  color: var(--el-text-color-secondary);
  font-family: monospace;
}

/* 名称输入框 */
.ws-label-input {
  flex: 1;
  max-width: 180px;
}

/* 跳转链接输入框 */
.ws-path-input {
  flex: 1;
  max-width: 220px;
}

/* 通用按钮样式 */
.ws-btn {
  padding: 6px 14px;
  font-size: 13px;
  border-radius: 4px;
  cursor: pointer;
  border: 1px solid transparent;
}

.ws-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

/* 删除按钮 */
.ws-delete-btn {
  color: #f56c6c;
  border-color: #f56c6c;
  background: transparent;
}

.ws-delete-btn:hover:not(:disabled) {
  color: #fff;
  background: #f56c6c;
}

/* 添加按钮 */
.ws-add-btn {
  color: #409eff;
  border-color: #409eff;
  background: transparent;
}

.ws-add-btn:hover {
  color: #fff;
  background: #409eff;
}

/* 操作区 */
.ws-actions {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-top: 12px;
}

/* 未保存提示 */
.ws-dirty-tip {
  font-size: 12px;
  color: #e6a23c;
}

/* 底部按钮区 */
.ws-footer {
  display: flex;
  gap: 10px;
  margin-top: 16px;
  padding-top: 12px;
  border-top: 1px solid #f0f0f0;
}

/* 保存按钮（主按钮） */
.ws-save-btn {
  color: #fff;
  background: #409eff;
}

.ws-save-btn:hover:not(:disabled) {
  background: #66b1ff;
}

/* 重置按钮 */
.ws-reset-btn {
  color: var(--el-text-color-regular);
  border-color: var(--el-border-color);
  background: var(--el-bg-color);
}

.ws-reset-btn:hover:not(:disabled) {
  color: #409eff;
  border-color: #409eff;
}
</style>
