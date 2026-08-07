// 路由配置：前台博客 + 后台管理（后台迁移到 /admin）
import { createRouter, createWebHashHistory } from 'vue-router'
import HomeView from '../views/HomeView.vue'
import LoginView from '../views/LoginView.vue'
import BlogHome from '../page/BlogHome.vue'
import ArticleDetail from '../page/ArticleDetail.vue'
import AboutView from '../page/AboutView.vue'
import Chat from '../page/Chat.vue'

const router = createRouter({
  // hash 模式：前端静态产物由 webcpp-engine 网关直接 serve，
  // 刷新子路由不依赖服务器 SPA fallback（history 模式需要，网关暂不支持）
  history: createWebHashHistory(import.meta.env.BASE_URL),
  routes: [
    { path: '/', name: 'home', component: BlogHome },
    { path: '/article/:id', name: 'article', component: ArticleDetail },
    { path: '/about', name: 'about', component: AboutView },
    { path: '/aichat', name: 'aichat', component: Chat },
    { path: '/admin', name: 'admin', component: HomeView, meta: { requiresAuth: true } },
    { path: '/login', name: 'login', component: LoginView }
  ]
})

// 登录 token 存储键（与 LoginView / HomeView 退出逻辑共用）
const TOKEN_KEY = 'blog_token'

/**
 * 全局前置守卫：后台路由需登录，未登录重定向登录页（带 redirect 回跳）；已登录访问登录页则进后台
 * @param to 目标路由对象
 * @returns 放行返回 true，或重定向到登录页 / 后台首页
 */
router.beforeEach((to) => {
  const hasToken = !!localStorage.getItem(TOKEN_KEY)
  if (to.meta.requiresAuth && !hasToken) {
    return { path: '/login', query: { redirect: to.fullPath } }
  }
  if (to.path === '/login' && hasToken) {
    return { path: '/admin' }
  }
  return true
})

export default router
