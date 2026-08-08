// 响应式视口断点检测：用 matchMedia 而非 resize 事件，避免频繁触发重算。
//
// 断点体系（与各组件 scoped style 的 @media 数值保持一致）：
//   - isMobile  ：视口 ≤ 768px（平板/手机分界，后台侧边栏折叠、弹窗全屏、表单上下排布）
//   - isCompact ：视口 ≤ 480px（小屏手机，进一步压缩 padding/字号/间距）
//
// 用法：const { isMobile } = useViewport()
//   <el-dialog :width="isMobile ? '100%' : '640px'" :fullscreen="isMobile" />
import { ref, onScopeDispose, type Ref } from 'vue'

export interface ViewportState {
  isMobile: Ref<boolean>
  isCompact: Ref<boolean>
}

/**
 * 视口断点响应式状态：SSR 不适用（本项目纯浏览器），匹配结果变化自动更新 ref
 * @returns { isMobile, isCompact } 两个响应式布尔 ref
 */
export function useViewport(): ViewportState {
  // matchMedia 在浏览器环境才存在；组件挂载前调用时兜底为桌面端
  const mobileQuery = window.matchMedia('(max-width: 768px)')
  const compactQuery = window.matchMedia('(max-width: 480px)')

  const isMobile = ref(mobileQuery.matches)
  const isCompact = ref(compactQuery.matches)

  const onMobileChange = (e: MediaQueryListEvent): void => {
    isMobile.value = e.matches
  }
  const onCompactChange = (e: MediaQueryListEvent): void => {
    isCompact.value = e.matches
  }

  // 现代浏览器用 addEventListener，旧版退化为 addListener（兼容兜底）
  if (typeof mobileQuery.addEventListener === 'function') {
    mobileQuery.addEventListener('change', onMobileChange)
    compactQuery.addEventListener('change', onCompactChange)
  } else {
    mobileQuery.addListener(onMobileChange)
    compactQuery.addListener(onCompactChange)
  }

  // 组件卸载时移除监听：后台面板切换销毁重建，不清理会持续累积监听器
  onScopeDispose(() => {
    if (typeof mobileQuery.removeEventListener === 'function') {
      mobileQuery.removeEventListener('change', onMobileChange)
      compactQuery.removeEventListener('change', onCompactChange)
    } else {
      mobileQuery.removeListener(onMobileChange)
      compactQuery.removeListener(onCompactChange)
    }
  })

  return { isMobile, isCompact }
}
