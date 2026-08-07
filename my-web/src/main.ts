import { createApp } from 'vue'
import App from './App.vue'
import ElementPlus from 'element-plus'
import 'element-plus/dist/index.css'
import 'element-plus/theme-chalk/dark/css-vars.css' // Element Plus 暗色变量
import './assets/theme.css' // 博客前台主题变量
import * as ElementPlusIconsVue from '@element-plus/icons-vue'
import router from './router'
import { blogConfigState, initBlogConfig } from './composables/useBlogConfig'

// 浏览器标签页标题跟随站点名：不再使用 Vite 默认标题，
// 以后台「站点设置 → 站点名称」为准，改配置后同步更新
const applyTitle = () => {
  document.title = blogConfigState.siteName || 'My Blog'
}
applyTitle()
// 跨标签页同步：其他页签修改 blogConfig 后，本页签标题跟着变
window.addEventListener('storage', applyTitle)
// 拉后端全局配置：站点名更新后（reactive 状态变化）重新应用标题
initBlogConfig().then(applyTitle)

const app = createApp(App)
for (const [key, component] of Object.entries(ElementPlusIconsVue)) {
  app.component(key, component)
}
app.use(ElementPlus)
app.use(router)
app.mount('#app')