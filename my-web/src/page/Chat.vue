<!-- src/page/Chat.vue AI 会话页（demo）：左侧历史会话 + 右侧聊天区，支持图片/文件上传，mock AI 回复 -->
<script lang="ts" setup>
import { ref, computed, watch, nextTick } from 'vue'
import { ElMessage, type UploadRequestOptions } from 'element-plus'
import { Document, Picture } from '@element-plus/icons-vue'
import BlogHeader from '../components/BlogHeader.vue'
import PageBackground from '../components/PageBackground.vue'
import { loadBlogConfig } from '../composables/useBlogConfig'

// ===== 数据结构 =====
interface ChatMessage {
  id: string
  role: 'user' | 'assistant'
  type: 'text' | 'image' | 'file'
  content: string
  fileUrl?: string // 图片 dataURL 或文件预览 URL
  fileName?: string
  createdAt: string
}

interface ChatSession {
  id: string
  title: string
  messages: ChatMessage[]
  createdAt: string
  updatedAt: string
}

const config = loadBlogConfig()

// ===== 会话持久化：localStorage =====
const SESSIONS_KEY = 'aichat_sessions'

/** 生成当前时间的 ISO 字符串 */
const nowISO = () => new Date().toISOString()

/** 生成唯一消息 id */
const uid = () => `m-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`

/**
 * 新建会话对象（默认标题「新会话」，首条消息后自动命名）
 * @returns 新创建的会话对象
 */
const createSession = (): ChatSession => {
  const t = nowISO()
  return { id: `s-${Date.now()}-${Math.random().toString(36).slice(2, 6)}`, title: '新会话', messages: [], createdAt: t, updatedAt: t }
}

/**
 * 从 localStorage 读取历史会话，无数据或数据损坏时返回默认会话
 * @returns 会话列表
 */
const loadSessions = (): ChatSession[] => {
  try {
    const saved = localStorage.getItem(SESSIONS_KEY)
    if (saved) {
      const list = JSON.parse(saved) as ChatSession[]
      if (list.length) return list
    }
  } catch {
    // localStorage 数据损坏时忽略，用默认会话
  }
  return [createSession()]
}

const sessions = ref<ChatSession[]>(loadSessions())
const activeId = ref<string>(sessions.value[0]?.id || '')

/**
 * 当前激活的会话（找不到时用历史会话第一项兜底，防止切换闪空）
 */
const activeSession = computed<ChatSession | undefined>(
  () => sessions.value.find((s) => s.id === activeId.value) || sessions.value[0]
)

/**
 * 将会话列表持久化到 localStorage，数据过大时给出提示
 */
const saveSessions = () => {
  try {
    localStorage.setItem(SESSIONS_KEY, JSON.stringify(sessions.value))
  } catch {
    ElMessage.warning('会话数据过大可能无法持久化，图片请尽量使用小文件')
  }
}

/**
 * 新建一个会话并置为当前激活会话
 */
const newSession = () => {
  const s = createSession()
  sessions.value.unshift(s)
  activeId.value = s.id
  saveSessions()
}

/**
 * 切换当前激活的历史会话
 * @param id 目标会话 id
 */
const selectSession = (id: string) => {
  activeId.value = id
}

// ===== 消息发送 =====
const input = ref('')
const sending = ref(false)
const messageListRef = ref<HTMLElement>()

/**
 * 滚动消息列表到底部（等待 DOM 更新后执行）
 */
const scrollToBottom = () => {
  nextTick(() => {
    const el = messageListRef.value
    if (el) el.scrollTop = el.scrollHeight
  })
}

/**
 * 追加一条用户消息并自动命名会话，随后持久化并滚动到底部
 * @param session 目标会话
 * @param msg 用户消息
 */
const appendUserMessage = (session: ChatSession, msg: ChatMessage) => {
  session.messages.push(msg)
  if (session.messages.length === 1) {
    session.title = (msg.fileName || msg.content).slice(0, 12)
  }
  session.updatedAt = nowISO()
  saveSessions()
  scrollToBottom()
}

/**
 * 生成 mock AI 回复文案（demo 占位，接入真实大模型后替换此处）
 * @param text 用户消息原文
 * @returns 回复文本
 */
const mockReply = (text: string) =>
  `已收到你的消息：「${text}」\n\n当前为 AI 会话 demo 页面，尚未接入真实大模型接口。后续接入后端后，可将此处的 mock 回复替换为真实 AI 应答。`

/**
 * 触发 AI 回复：设置加载态，延迟后追加 assistant 消息并持久化
 * @param session 目标会话
 * @param userText 用户消息文本（用于生成 mock 回复）
 * @returns 无返回值（异步执行）
 */
const triggerReply = async (session: ChatSession, userText: string) => {
  sending.value = true
  await new Promise((r) => setTimeout(r, 700))
  const reply: ChatMessage = {
    id: uid(),
    role: 'assistant',
    type: 'text',
    content: mockReply(userText),
    createdAt: nowISO()
  }
  session.messages.push(reply)
  session.updatedAt = nowISO()
  saveSessions()
  sending.value = false
  scrollToBottom()
}

/**
 * 发送文本消息（回车或点击发送按钮触发）
 * @returns 无返回值（异步执行）
 */
const sendMessage = async () => {
  const text = input.value.trim()
  if (!text || sending.value) return
  input.value = ''
  const session = activeSession.value
  if (!session) return
  appendUserMessage(session, { id: uid(), role: 'user', type: 'text', content: text, createdAt: nowISO() })
  await triggerReply(session, text)
}

/**
 * 将上传的图片/文件作为用户消息追加并触发 AI 回复
 * @param payload 媒体消息数据（类型、内容、预览地址、文件名）
 */
const pushMedia = (payload: { type: 'image' | 'file'; content: string; fileUrl: string; fileName: string }) => {
  const session = activeSession.value
  if (!session) return
  appendUserMessage(session, { id: uid(), role: 'user', type: payload.type, content: payload.content, fileUrl: payload.fileUrl, fileName: payload.fileName, createdAt: nowISO() })
  triggerReply(session, payload.fileName || payload.content)
}

/**
 * 图片上传处理：FileReader 转 base64 存进消息（刷新后仍可预览）
 * @param options Element Plus 上传请求配置（含文件与完成回调）
 */
const handleUploadImage = (options: UploadRequestOptions) => {
  const file = options.file
  const reader = new FileReader()
  reader.onload = () => {
    pushMedia({ type: 'image', content: `[图片] ${file.name}`, fileUrl: String(reader.result), fileName: file.name })
    options.onSuccess({})
  }
  reader.readAsDataURL(file)
}

/**
 * 文件上传处理：用 objectURL 预览（刷新后失效，仅保留文件名展示）
 * @param options Element Plus 上传请求配置（含文件与完成回调）
 */
const handleUploadFile = (options: UploadRequestOptions) => {
  const file = options.file
  const url = URL.createObjectURL(file)
  pushMedia({ type: 'file', content: `[文件] ${file.name}`, fileUrl: url, fileName: file.name })
  options.onSuccess({})
}

/**
 * 会话时间格式化显示：MM-DD HH:MM
 * @param iso ISO 时间字符串
 * @returns 格式化后的时间文本
 */
const formatTime = (iso: string) => {
  const d = new Date(iso)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`
}

// 切换会话后滚动到最新消息
watch(activeId, () => scrollToBottom())
</script>

<template>
  <div class="chat-page">
    <PageBackground :background="config.background" />
    <BlogHeader :title="config.siteName" />

    <main class="chat-main">
      <div class="chat-layout">
        <!-- 左侧：历史会话列表 -->
        <aside class="chat-sidebar">
          <div class="sidebar-header">
            <span class="sidebar-title">历史会话</span>
            <el-button type="primary" size="small" @click="newSession">+ 新会话</el-button>
          </div>
          <div class="session-list">
            <div
              v-for="s in sessions"
              :key="s.id"
              class="session-item"
              :class="{ active: s.id === activeId }"
              @click="selectSession(s.id)"
            >
              <div class="session-title">{{ s.title || '新会话' }}</div>
              <div class="session-time">{{ formatTime(s.updatedAt) }}</div>
            </div>
          </div>
        </aside>

        <!-- 右侧：聊天区 -->
        <section class="chat-body">
          <div class="chat-header">
            <span class="chat-header-title">{{ activeSession?.title || '新会话' }}</span>
          </div>

          <!-- 消息列表 -->
          <div ref="messageListRef" class="message-list">
            <el-empty v-if="!activeSession?.messages.length" description="开始新的对话吧，支持文字、图片和文件" />
            <div v-for="msg in activeSession?.messages" :key="msg.id" class="message-row" :class="msg.role">
              <div class="message-bubble" :class="msg.type">
                <img v-if="msg.type === 'image'" :src="msg.fileUrl" class="message-image" alt="图片" />
                <div v-else-if="msg.type === 'file'" class="message-file">
                  <el-icon><Document /></el-icon>
                  <a :href="msg.fileUrl" download>{{ msg.fileName }}</a>
                </div>
                <span v-else class="message-text">{{ msg.content }}</span>
              </div>
            </div>
            <!-- AI 加载占位 -->
            <div v-if="sending" class="message-row assistant">
              <div class="message-bubble">
                <span class="message-text message-typing">正在输入…</span>
              </div>
            </div>
          </div>

          <!-- 输入区：上传 + 输入 + 发送 -->
          <div class="chat-input-area">
            <div class="chat-toolbar">
              <el-upload :show-file-list="false" :http-request="handleUploadImage" accept="image/*">
                <el-button size="small" :icon="Picture">图片</el-button>
              </el-upload>
              <el-upload :show-file-list="false" :http-request="handleUploadFile">
                <el-button size="small" :icon="Document">文件</el-button>
              </el-upload>
            </div>
            <div class="chat-input-row">
              <el-input
                v-model="input"
                type="textarea"
                :rows="2"
                resize="none"
                placeholder="输入消息，Enter 发送，Shift+Enter 换行"
                @keydown.enter.exact.prevent="sendMessage()"
              />
              <el-button type="primary" :loading="sending" @click="sendMessage()">发送</el-button>
            </div>
          </div>
        </section>
      </div>
    </main>
  </div>
</template>

<style scoped>
/* 页面骨架：顶部导航吸顶，聊天区占满剩余高度 */
.chat-page {
  height: 100vh;
  display: flex;
  flex-direction: column;
}

.chat-main {
  flex: 1;
  min-height: 0;
  width: 100%;
  max-width: 1200px;
  margin: 0 auto;
  padding: 16px 24px;
  box-sizing: border-box;
}

/* 左右布局：侧边栏 + 聊天区 */
.chat-layout {
  display: flex;
  gap: 16px;
  height: 100%;
}

/* ===== 左侧历史会话 ===== */
.chat-sidebar {
  width: 260px;
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border: 1px solid var(--blog-border);
  border-radius: 8px;
  background: var(--blog-card);
}

.sidebar-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px;
  border-bottom: 1px solid var(--blog-border);
}

.sidebar-title {
  font-size: 14px;
  font-weight: 600;
  color: var(--blog-text);
}

.session-list {
  flex: 1;
  overflow-y: auto;
  padding: 8px;
}

.session-item {
  padding: 10px 12px;
  border-radius: 6px;
  cursor: pointer;
  transition: background-color 0.2s;
}

.session-item:hover {
  background: var(--blog-bg-secondary);
}

.session-item.active {
  background: var(--blog-primary-light);
}

.session-title {
  font-size: 13px;
  color: var(--blog-text);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.session-time {
  margin-top: 4px;
  font-size: 11px;
  color: var(--blog-text-secondary);
}

/* ===== 右侧聊天区 ===== */
.chat-body {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border: 1px solid var(--blog-border);
  border-radius: 8px;
  background: var(--blog-card);
}

.chat-header {
  padding: 12px 16px;
  border-bottom: 1px solid var(--blog-border);
  font-size: 14px;
  font-weight: 600;
  color: var(--blog-text);
}

.message-list {
  flex: 1;
  overflow-y: auto;
  padding: 20px;
}

.message-row {
  display: flex;
  margin-bottom: 16px;
}

.message-row.user {
  justify-content: flex-end;
}

.message-row.assistant {
  justify-content: flex-start;
}

.message-bubble {
  max-width: 70%;
  padding: 10px 14px;
  border-radius: 8px;
  font-size: 14px;
  line-height: 1.6;
  white-space: pre-wrap;
  word-break: break-word;
}

.message-row.user .message-bubble {
  background: var(--blog-primary);
  color: #fff;
}

.message-row.assistant .message-bubble {
  background: var(--blog-bg-secondary);
  color: var(--blog-text);
}

/* 图片/文件消息：不带气泡底色 */
.message-bubble.image,
.message-bubble.file {
  padding: 0;
  background: transparent;
}

.message-image {
  max-width: 260px;
  max-height: 200px;
  border-radius: 6px;
  display: block;
}

.message-file {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 8px 12px;
  border-radius: 6px;
  background: var(--blog-bg-secondary);
  color: var(--blog-text);
}

.message-file .el-icon {
  font-size: 18px;
  color: var(--blog-primary);
}

.message-file a {
  color: var(--blog-text);
  text-decoration: none;
}

.message-file a:hover {
  color: var(--blog-primary);
}

.message-typing {
  color: var(--blog-text-secondary);
}

/* ===== 输入区 ===== */
.chat-input-area {
  border-top: 1px solid var(--blog-border);
  padding: 12px;
}

.chat-toolbar {
  margin-bottom: 8px;
  display: flex;
  gap: 8px;
}

.chat-input-row {
  display: flex;
  gap: 8px;
  align-items: flex-end;
}

.chat-input-row .el-textarea {
  flex: 1;
}
</style>
