<!-- src/views/LoginView.vue 登录页 -->
<template>
  <div class="login-page" :style="pageBgStyle">
    <!-- 返回首页 -->
    <router-link to="/" class="back-home">
      <el-icon><ArrowLeft /></el-icon>
      返回首页
    </router-link>

    <div class="login-card" :style="cardBgStyle">
      <!-- 标题区 -->
      <div class="login-header">
        <div class="login-logo">
          <el-icon :size="28"><Lock /></el-icon>
        </div>
        <h2 class="login-title">系统登录</h2>
        <p class="login-subtitle">欢迎回来，请登录后台管理系统</p>
      </div>

      <!-- 登录表单 -->
      <el-form
        ref="formRef"
        :model="form"
        :rules="rules"
        size="large"
        @submit.prevent
      >
        <el-form-item prop="username">
          <el-input
            v-model="form.username"
            placeholder="用户名"
            :prefix-icon="User"
            clearable
          />
        </el-form-item>

        <el-form-item prop="password">
          <el-input
            v-model="form.password"
            type="password"
            placeholder="密码"
            :prefix-icon="Lock"
            show-password
            @keyup.enter="handleLogin"
          />
        </el-form-item>

        <!-- 记住我 + 忘记密码 -->
        <div class="login-options">
          <el-checkbox v-model="form.remember">记住我</el-checkbox>
          <el-link type="primary" :underline="false">忘记密码？</el-link>
        </div>

        <el-button
          type="primary"
          class="login-btn"
          :loading="loading"
          @click="handleLogin"
        >
          {{ loading ? '登录中...' : '登 录' }}
        </el-button>
      </el-form>

      <!-- 底部提示 -->
      <div class="login-footer">
        还没有账号？
        <el-link type="primary" :underline="false">立即注册</el-link>
      </div>
    </div>
  </div>
</template>

<script lang="ts" setup>
import { ref, reactive, computed } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { ElMessage, type FormInstance, type FormRules } from 'element-plus'
import { User, Lock } from '@element-plus/icons-vue'
import { login } from '../api/blog'

const router = useRouter()
const route = useRoute()

// 读取外观配置（由 Appearance 面板设置后持久化到 localStorage）
const loginBg = localStorage.getItem('loginBgImage') || ''
const loginCardBg = localStorage.getItem('loginCardBgImage') || ''

/**
 * 登录页背景样式：设置了背景图则覆盖显示，否则返回空对象使用默认渐变
 */
const pageBgStyle = computed(() =>
  loginBg
    ? {
        backgroundImage: `url(${loginBg})`,
        backgroundSize: 'cover',
        backgroundPosition: 'center'
      }
    : {}
)

/**
 * 登录框背景样式：设置了背景图则覆盖显示，否则返回空对象使用默认白色
 */
const cardBgStyle = computed(() =>
  loginCardBg
    ? {
        backgroundImage: `url(${loginCardBg})`,
        backgroundSize: 'cover',
        backgroundPosition: 'center'
      }
    : {}
)

// 表单数据
const formRef = ref<FormInstance>()
const loading = ref(false)
const form = reactive({
  username: '',
  password: '',
  remember: true
})

// 表单校验规则
const rules: FormRules = {
  username: [{ required: true, message: '请输入用户名', trigger: 'blur' }],
  password: [
    { required: true, message: '请输入密码', trigger: 'blur' },
    { min: 6, message: '密码至少 6 位', trigger: 'blur' }
  ]
}

/**
 * 登录：表单校验通过后调用登录接口，成功后持久化 token 并跳转后台（支持 redirect 回跳）
 * @returns 无返回值（异步执行，成功/失败均有提示）
 */
const handleLogin = async () => {
  const valid = await formRef.value?.validate().catch(() => false)
  if (!valid) return

  loading.value = true
  try {
    const { token, username } = await login(form.username, form.password)
    // 持久化登录态：路由守卫据此放行 /admin；登出时清除
    localStorage.setItem('blog_token', token)
    localStorage.setItem('blog_username', username)
    if (form.remember) {
      localStorage.setItem('username', form.username)
    }
    ElMessage.success('登录成功')
    // 未登录时访问后台会被守卫带 redirect 参数过来，登录后原路返回；否则进后台首页
    const redirect = typeof route.query.redirect === 'string' ? route.query.redirect : '/admin'
    router.push(redirect)
  } catch (err) {
    ElMessage.error(err instanceof Error ? err.message : '登录失败')
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
/* 登录页：渐变背景 + 全屏居中 */
.login-page {
  position: relative;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
}

/* 返回首页链接 */
.back-home {
  position: absolute;
  top: 24px;
  left: 24px;
  display: flex;
  align-items: center;
  gap: 4px;
  color: #fff;
  font-size: 14px;
  text-decoration: none;
  opacity: 0.85;
}

.back-home:hover {
  opacity: 1;
}

/* 登录卡片 */
.login-card {
  width: 400px;
  padding: 40px;
  background: #fff;
  border-radius: 12px;
  box-shadow: 0 8px 40px rgba(0, 0, 0, 0.15);
}

/* 标题区 */
.login-header {
  text-align: center;
  margin-bottom: 28px;
}

/* 圆形 logo 图标 */
.login-logo {
  width: 56px;
  height: 56px;
  margin: 0 auto 12px;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #fff;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  border-radius: 50%;
}

.login-title {
  margin: 0;
  font-size: 22px;
  color: var(--el-text-color-primary);
}

.login-subtitle {
  margin: 6px 0 0;
  font-size: 13px;
  color: var(--el-text-color-secondary);
}

/* 记住我 + 忘记密码 */
.login-options {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}

/* 登录按钮 */
.login-btn {
  width: 100%;
  font-size: 15px;
  letter-spacing: 4px;
}

/* 底部注册提示 */
.login-footer {
  margin-top: 24px;
  text-align: center;
  font-size: 13px;
  color: var(--el-text-color-secondary);
}

/* ══════ 响应式：≤768px 移动端 ══════
   固定 400px 卡片改为流式全宽，两侧留 16px 边距，避免超过手机屏宽 */
@media (max-width: 768px) {
  /* flex:1 拉伸占满减去两侧边距，max-width 封顶，避免居中布局下 100%+margin 溢出 */
  .login-card {
    width: auto;
    flex: 1;
    max-width: 400px;
    padding: 28px 20px;
    margin: 0 16px;
  }

  /* 返回首页链接贴近边缘，避免遮挡 */
  .back-home {
    top: 16px;
    left: 16px;
    font-size: 13px;
  }
}
</style>
