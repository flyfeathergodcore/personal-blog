// src/api/http.ts 统一的 HTTP 请求封装（基于原生 fetch，零依赖）
// 自动携带 blog_token；401 清登录态跳登录页；非 2xx 抛后端返回的 message
const BASE_URL = import.meta.env.VITE_API_BASE || '/api'

// 登录 token 存储键（与 router 守卫 / LoginView 共用）
const TOKEN_KEY = 'blog_token'

async function request<T>(url: string, options: RequestInit = {}): Promise<T> {
  const token = localStorage.getItem(TOKEN_KEY)
  const headers: Record<string, string> = {
    ...(options.headers as Record<string, string> | undefined),
  }
  if (token) headers.Authorization = `Bearer ${token}`

  const res = await fetch(`${BASE_URL}${url}`, { ...options, headers })

  // 401：token 失效 / 未登录，清登录态回登录页（登录页自身不跳，避免死循环）
  if (res.status === 401) {
    localStorage.removeItem(TOKEN_KEY)
    localStorage.removeItem('blog_username')
    // hash 路由：真实路由在 location.hash 里（pathname 恒为 /），不在登录页才跳转
    const cur = window.location.hash || '#/'
    if (!cur.startsWith('#/login')) {
      const redirect = encodeURIComponent(cur.slice(1) || '/admin')
      window.location.href = `/#/login?redirect=${redirect}`
    }
    throw new Error('登录已过期，请重新登录')
  }

  // 非 2xx：抛出后端返回的错误信息
  if (!res.ok) {
    const body = (await res.json().catch(() => ({}))) as { message?: string }
    throw new Error(body.message || `请求失败（${res.status}）`)
  }

  return res.json() as Promise<T>
}

// 需要拿到"未找到"（404 → null）时用 getOptional（如文章详情）
async function requestOptional<T>(url: string): Promise<T | null> {
  const res = await fetch(`${BASE_URL}${url}`, {
    headers: (localStorage.getItem(TOKEN_KEY)
      ? { Authorization: `Bearer ${localStorage.getItem(TOKEN_KEY)}` }
      : {}) as Record<string, string>,
  })
  if (res.status === 404) return null
  if (!res.ok) {
    const body = (await res.json().catch(() => ({}))) as { message?: string }
    throw new Error(body.message || `请求失败（${res.status}）`)
  }
  return res.json() as Promise<T>
}

export const http = {
  get: <T>(url: string) => request<T>(url),
  getOptional: <T>(url: string) => requestOptional<T>(url),
  post: <T>(url: string, data?: unknown) =>
    request<T>(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: data === undefined ? undefined : JSON.stringify(data),
    }),
  put: <T>(url: string, data?: unknown) =>
    request<T>(url, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: data === undefined ? undefined : JSON.stringify(data),
    }),
  delete: <T>(url: string) => request<T>(url, { method: 'DELETE' }),
  // 文件上传：FormData，浏览器自动带 multipart boundary。
  // fetch 不提供上传进度，故用 XHR 实现；onProgress 回调 0-100 整数百分比
  upload: <T>(url: string, file: File, onProgress?: (percent: number) => void) =>
    new Promise<T>((resolve, reject) => {
      const xhr = new XMLHttpRequest()
      xhr.open('POST', `${BASE_URL}${url}`)
      const token = localStorage.getItem(TOKEN_KEY)
      if (token) xhr.setRequestHeader('Authorization', `Bearer ${token}`)
      if (onProgress) {
        xhr.upload.onprogress = (e) => {
          if (e.lengthComputable)
            onProgress(Math.round((e.loaded / e.total) * 100))
        }
      }
      xhr.onload = () => {
        // 401：清登录态跳登录页（与 request 逻辑一致）
        if (xhr.status === 401) {
          localStorage.removeItem(TOKEN_KEY)
          localStorage.removeItem('blog_username')
          const cur = window.location.hash || '#/'
          if (!cur.startsWith('#/login')) window.location.href = '/#/login'
          reject(new Error('登录已过期，请重新登录'))
          return
        }
        let body: T | null = null
        try {
          body = JSON.parse(xhr.responseText) as T
        } catch {
          /* 空响应不做解析 */
        }
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(body as T)
        } else {
          const msg =
            (body as { message?: string } | null)?.message ||
            `请求失败（${xhr.status}）`
          reject(new Error(msg))
        }
      }
      xhr.onerror = () => reject(new Error('网络错误，上传失败'))
      xhr.ontimeout = () => reject(new Error('上传超时，请重试'))
      // 大文件上传给足时间（默认 0 表示不超时，用 2 分钟兜底避免无限等待）
      xhr.timeout = 120000
      const form = new FormData()
      form.append('file', file)
      xhr.send(form)
    }),
}
