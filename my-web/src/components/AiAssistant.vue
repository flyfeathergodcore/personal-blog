<!-- src/components/AiAssistant.vue 虚拟人物占位按钮：右下角浮窗，面板为占位文案 -->
<template>
  <div v-if="enabled" class="ai-assistant">
    <el-popover placement="top-end" :width="260" trigger="click">
      <template #reference>
        <el-button type="primary" circle class="ai-fab" aria-label="AI 助手">
          <el-icon :size="20"><ChatDotRound /></el-icon>
        </el-button>
      </template>
      <div class="ai-panel">
        <p class="ai-title">🤖 AI 助手</p>
        <p class="ai-desc">{{ placeholder }}</p>
      </div>
    </el-popover>
  </div>
</template>

<script lang="ts" setup>
import { ChatDotRound } from '@element-plus/icons-vue'

defineProps({
  enabled: { type: Boolean, default: true },
  placeholder: {
    type: String,
    default: 'AI 助手开发中。后续支持框选文档文字与 AI 交互，生成结果将插入到文档展示中。'
  }
})

// 预留后期扩展点：框选文字输入 / 结果插入文档的方法，供父组件（MdViewer 场景）调用
// TODO(后期): 接入真实 AI 交互后实现具体逻辑
defineExpose({
  insertToDocument: (text: string): void => {
    console.log('AI 结果插入占位:', text)
  }
})
</script>

<style scoped>
.ai-assistant {
  position: fixed;
  right: 24px;
  bottom: 24px;
  z-index: 100;
}

.ai-fab {
  width: 48px;
  height: 48px;
  box-shadow: var(--blog-shadow);
}

.ai-title {
  margin: 0 0 8px;
  font-size: 14px;
  font-weight: 600;
}

.ai-desc {
  margin: 0;
  font-size: 13px;
  line-height: 1.6;
  color: var(--blog-text-secondary);
}
</style>
