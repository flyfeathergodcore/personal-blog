// 指标参数配置读写：后台「站点设置 → 指标与统计」写入、Dashboard 轮询周期读取
//
// 数据源分层：
//   - 后端 /api/metrics-config 是事实源（存 site_config，热生效，重启回落
//     config.yaml metrics: 段默认值）。
//   - 前端不做本地缓存：Dashboard/SiteSettings 挂载时 loadMetricsConfig()
//     拉一次后端，失败时静默用 defaultMetricsConfig 兜底（不影响页面可用）。
//   - 保存成功后 Object.assign 同步本地响应式状态，Dashboard 下次挂载
//     自动读到新值（HomeView 面板切换销毁重建）。
import { reactive } from 'vue'
import { getMetricsConfig, saveMetricsConfig, type MetricsConfig } from '../api/blog'

// 默认值（与后端 config.hpp MetricsConfig / metrics.hpp RuntimeMetricsConfig
// 保持一致，三处同步，改一处须同改另两处）
export const defaultMetricsConfig: MetricsConfig = {
  flush_interval_ms: 1000, // QPS 刷新周期（Flush 间隔，100–1000ms）
  persist_interval_secs: 60, // 统计落库周期（10–3600s）
  cleanup_interval_secs: 3600, // 过期统计清理周期（300–86400s）
  cleanup_retention_days: 30, // 清理保留窗口（7–3650 天）
  realtime_refresh_ms: 3000, // 仪表盘实时轮询（1000–60000ms）
  trend_refresh_ms: 60000, // 仪表盘趋势图轮询（5000–3600000ms）
  visitor_refresh_ms: 5000, // 仪表盘访问者表轮询（1000–60000ms）
}

// 响应式全局状态：Dashboard 轮询周期与 SiteSettings 表单都从这里读
export const metricsConfigState = reactive<MetricsConfig>({ ...defaultMetricsConfig })

/**
 * 从后端拉取指标参数并同步到本地状态；失败时静默用默认值（不阻塞页面）
 * @returns 无返回值（异步执行）
 */
export const loadMetricsConfig = async (): Promise<void> => {
  try {
    const cfg = await getMetricsConfig()
    Object.assign(metricsConfigState, cfg)
  } catch {
    Object.assign(metricsConfigState, defaultMetricsConfig)
  }
}

/**
 * 保存指标参数到后端（后端 clamp + 热应用 + 落库），成功后同步本地状态
 * @param cfg 指标参数（支持部分字段，缺失回落当前运行时值）
 * @returns 后端 clamp 后的完整配置（可直接用于表单回显）
 */
export const saveMetricsConfigAsync = async (cfg: Partial<MetricsConfig>): Promise<MetricsConfig> => {
  const saved = await saveMetricsConfig(cfg)
  Object.assign(metricsConfigState, saved)
  return saved
}
