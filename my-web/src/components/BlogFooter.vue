<!-- src/components/BlogFooter.vue 博客页脚：版权 + 友情链接，数据由外部传入 -->
<template>
  <footer class="blog-footer">
    <div v-if="links.length" class="footer-links">
      <el-link v-for="l in links" :key="l.label" :href="l.url" target="_blank">{{ l.label }}</el-link>
    </div>
    <el-divider />
    <p class="footer-copyright">{{ copyright }}</p>
  </footer>
</template>

<script lang="ts" setup>
import type { PropType } from 'vue'

interface FooterLink {
  label: string
  url: string
}

defineProps({
  copyright: { type: String, default: '© 2026 My Blog' },
  links: { type: Array as PropType<FooterLink[]>, default: () => [] }
})
</script>

<style scoped>
.blog-footer {
  padding: 32px 0 16px;
  text-align: center;
  color: var(--blog-text-secondary);
}

.footer-links {
  display: flex;
  justify-content: center;
  gap: 24px;
  flex-wrap: wrap;
}

.footer-copyright {
  margin: 0;
  font-size: 13px;
}
</style>
