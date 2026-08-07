<!-- src/components/PageBackground.vue 主页背景：渲染后台配置的背景，置于内容之下 -->
<template>
  <div class="page-bg" :style="bgStyle"></div>
</template>

<script lang="ts" setup>
import { computed } from 'vue'

const props = defineProps({
  // CSS background 值或图片 URL，留空走主题默认背景
  background: { type: String, default: '' },
  // 有背景图时叠加浅色遮罩，保证前景文字可读
  overlay: { type: Boolean, default: true }
})

const bgStyle = computed(() => {
  const value = props.background
  if (!value) return { background: 'var(--blog-bg)' }
  const isImage = !value.startsWith('#') && !value.includes('gradient')
  const img = isImage ? `url(${value}) center/cover no-repeat` : value
  // 遮罩：图片上叠一层半透明主题色（--blog-overlay），
  // 降低对比保证前景可读，但半透明不能完全盖住背景图
  const mask = props.overlay && isImage
    ? `linear-gradient(var(--blog-overlay), var(--blog-overlay))`
    : ''
  return { background: mask ? `${mask}, ${img}` : img }
})
</script>

<style scoped>
/* 固定铺满视口，置于所有内容之下（z-index: -1） */
.page-bg {
  position: fixed;
  inset: 0;
  z-index: -1;
}
</style>
