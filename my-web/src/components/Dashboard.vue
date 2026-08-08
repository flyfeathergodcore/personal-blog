<!-- src/components/Dashboard.vue 后台仪表盘：站点访问频率（PV/QPS）+ 负载指标监控
     实时层：每 3s 轮询 /metrics.json（后端 60s 环形缓冲，同源接口）
     历史层：/api/stats（site_stats 分钟落库，近 24h/7d 趋势） -->
<template>
  <div class="dashboard">
    <!-- 实时数字卡片 -->
    <div class="stat-cards">
      <div class="stat-card" v-for="c in cards" :key="c.label">
        <div class="stat-value">{{ c.value }}</div>
        <div class="stat-label">{{ c.label }}</div>
      </div>
    </div>

    <!-- 近 60s 实时 QPS 曲线 -->
    <div class="chart-box">
      <h4>实时 QPS（近 60s）</h4>
      <div ref="realtimeEl" class="chart"></div>
    </div>

    <!-- 历史访问趋势（落库 site_stats） -->
    <div class="chart-box">
      <div class="chart-head">
        <h4>历史访问趋势</h4>
        <el-radio-group v-model="range" size="small" @change="loadStats">
          <el-radio-button value="24h">近 24 小时</el-radio-button>
          <el-radio-button value="7d">近 7 天</el-radio-button>
        </el-radio-group>
      </div>
      <div ref="trendEl" class="chart"></div>
    </div>

    <!-- 访问者 IP：当前在线 IP（后端自追踪，最近 120s 内有请求才显示，离线即消失） -->
    <div class="chart-box">
      <div class="chart-head">
        <h4>访问者 IP</h4>
        <span class="visitor-online">🟢 当前在线 <b>{{ visitorOnlineCount }}</b> 个 IP</span>
      </div>
      <!-- 表格横向滚动容器：窄屏下 IP 列不溢出 -->
      <div class="table-scroll">
        <el-table
          :data="visitorList"
          size="small"
          v-loading="visitorLoading"
          empty-text="当前无在线 IP（最近 120s 内有请求的设备才会出现在这里）"
        >
          <el-table-column prop="ip" label="IP 地址" min-width="150" />
          <el-table-column prop="lastSeen" label="最近活跃" min-width="160" />
        </el-table>
      </div>
    </div>
  </div>
</template>

<script lang="ts" setup>
import { onMounted, onBeforeUnmount, ref, type Ref } from 'vue'
import * as echarts from 'echarts'
import { getStats, type StatsResult, getVisitors, type VisitorInfo } from '../api/blog'
import { loadMetricsConfig, metricsConfigState } from '../composables/useMetricsConfig'

// 实时数据源 /metrics.json 的结构（只取用到字段）
interface MetricsJson {
  active_connections: number
  uptime_seconds: number
  history: {
    t: number
    qps: number
    err: number
    p99: number
  }[]
}

const cards = ref([
  { label: '当前 QPS', value: '—' },
  { label: '活动连接', value: '—' },
  { label: 'p99 延迟(μs)', value: '—' },
  { label: '错误率', value: '—' },
  { label: '近 60s 请求', value: '—' },
  { label: '运行时长', value: '—' }
])

const range = ref<'24h' | '7d'>('24h')
const realtimeEl: Ref<HTMLDivElement | null> = ref(null)
const trendEl: Ref<HTMLDivElement | null> = ref(null)

let realtimeChart: echarts.ECharts | null = null
let trendChart: echarts.ECharts | null = null
let realtimeTimer = 0
let trendTimer = 0
let visitorTimer = 0

// 访问者 IP：当前在线 IP 列表 + 在线数（5s 轮询）
const visitorList = ref<VisitorInfo[]>([])
const visitorOnlineCount = ref(0)
const visitorLoading = ref(false)

/**
 * 拉取当前在线访问者 IP（5s 轮询）；接口异常静默，保留上次数据
 */
const loadVisitors = async (): Promise<void> => {
  visitorLoading.value = true
  try {
    const res = await getVisitors(20)
    visitorList.value = res.visitors || []
    visitorOnlineCount.value = res.onlineCount || 0
  } catch {
    // 接口异常静默，保留上次数据
  } finally {
    visitorLoading.value = false
  }
}

/**
 * 运行时长格式化：x天x时 / x时x分 / x分
 * @param s 秒数
 * @returns 格式化后的时长字符串
 */
const fmtUptime = (s: number): string => {
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  return d > 0 ? `${d}天${h}时` : h > 0 ? `${h}时${m}分` : `${m}分`
}

/**
 * 渲染实时指标卡片 + 近 60s QPS 曲线（ECharts setOption）
 * @param m /metrics.json 解析出的实时数据
 */
const renderRealtime = (m: MetricsJson): void => {
  const hist = m.history || []
  const last = hist[hist.length - 1]
  const totalReq = hist.reduce((s, h) => s + h.qps, 0)
  const totalErr = hist.reduce((s, h) => s + h.err, 0)
  cards.value = [
    { label: '当前 QPS', value: last ? String(last.qps) : '0' },
    { label: '活动连接', value: String(m.active_connections ?? 0) },
    { label: 'p99 延迟(μs)', value: last ? String(last.p99) : '—' },
    {
      label: '错误率',
      value: totalReq ? `${((totalErr / totalReq) * 100).toFixed(2)}%` : '0%'
    },
    { label: '近 60s 请求', value: String(totalReq) },
    { label: '运行时长', value: fmtUptime(m.uptime_seconds || 0) }
  ]
  realtimeChart?.setOption({
    tooltip: { trigger: 'axis' },
    grid: { left: 44, right: 16, top: 24, bottom: 24 },
    xAxis: {
      type: 'category',
      data: hist.map((h) => new Date(h.t * 1000).toLocaleTimeString())
    },
    yAxis: { type: 'value', minInterval: 1 },
    series: [
      {
        type: 'line',
        smooth: true,
        areaStyle: { opacity: 0.2 },
        data: hist.map((h) => h.qps)
      }
    ]
  })
}

/**
 * 加载并渲染历史访问趋势（近 24h/7d）；无落库数据/接口异常时清空图表
 */
const loadStats = async (): Promise<void> => {
  try {
    const res: StatsResult = await getStats(range.value)
    const points = res.points || []
    trendChart?.setOption({
      tooltip: { trigger: 'axis' },
      legend: { data: ['请求数', '错误数'] },
      grid: { left: 44, right: 44, top: 40, bottom: 24 },
      xAxis: { type: 'category', data: points.map((p) => p.ts) },
      yAxis: [
        { type: 'value', name: '请求', minInterval: 1 },
        { type: 'value', name: '错误', minInterval: 1 }
      ],
      series: [
        {
          name: '请求数',
          type: 'line',
          smooth: true,
          areaStyle: { opacity: 0.15 },
          data: points.map((p) => p.req)
        },
        {
          name: '错误数',
          type: 'bar',
          yAxisIndex: 1,
          data: points.map((p) => p.err)
        }
      ]
    })
  } catch {
    // 尚无落库数据 / 接口异常：清空图表，卡片区保持空态
    trendChart?.clear()
  }
}

/**
 * 窗口尺寸变化时让实时与趋势图表自适应
 */
const resize = (): void => {
  realtimeChart?.resize()
  trendChart?.resize()
}

/**
 * 生命周期：初始化图表并启动实时/历史/访问者三类轮询定时器
 */
onMounted(async () => {
  realtimeChart = echarts.init(realtimeEl.value as HTMLDivElement)
  trendChart = echarts.init(trendEl.value as HTMLDivElement)

  // 先拉取指标参数（轮询周期 / 后端热配置），失败静默用默认值兜底
  await loadMetricsConfig()

  /**
   * 实时轮询：立即拉一次 + 每 realtime_refresh_ms 刷新（失败静默，卡片保持「—」）
   */
  const pollRealtime = async (): Promise<void> => {
    try {
      const r = await fetch('/metrics.json')
      if (r.ok) renderRealtime((await r.json()) as MetricsJson)
    } catch {
      /* 轮询失败静默重试 */
    }
  }
  void pollRealtime()
  realtimeTimer = window.setInterval(
    () => void pollRealtime(), metricsConfigState.realtime_refresh_ms)

  // 历史趋势：立即拉一次 + 每 trend_refresh_ms 刷新
  void loadStats()
  trendTimer = window.setInterval(
    () => void loadStats(), metricsConfigState.trend_refresh_ms)

  // 访问者 IP：立即拉一次 + 每 visitor_refresh_ms 刷新（在线状态实时性）
  void loadVisitors()
  visitorTimer = window.setInterval(
    () => void loadVisitors(), metricsConfigState.visitor_refresh_ms)

  window.addEventListener('resize', resize)
})

/**
 * 生命周期：清理全部轮询定时器、移除 resize 监听并销毁图表实例
 */
onBeforeUnmount(() => {
  window.clearInterval(realtimeTimer)
  window.clearInterval(trendTimer)
  window.clearInterval(visitorTimer)
  window.removeEventListener('resize', resize)
  realtimeChart?.dispose()
  trendChart?.dispose()
})
</script>

<style scoped>
.dashboard { max-width: 100%; }
.stat-cards { display: flex; flex-wrap: wrap; gap: 16px; margin-bottom: 24px; }
.stat-card {
  flex: 1 1 160px;
  padding: 16px 20px;
  background: var(--el-bg-color);
  border: 1px solid var(--el-border-color-light);
  border-radius: 8px;
}
.stat-value { font-size: 26px; font-weight: 600; color: var(--el-color-primary); }
.stat-label { margin-top: 6px; font-size: 12px; color: var(--el-text-color-secondary); }
.chart-box {
  background: var(--el-bg-color);
  border: 1px solid var(--el-border-color-light);
  border-radius: 8px;
  padding: 16px;
  margin-bottom: 24px;
}
.chart-box h4 { margin: 0 0 12px; }
.chart-head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
.chart-head h4 { margin: 0; }
.chart { height: 280px; }
.visitor-online { font-size: 12px; color: var(--el-text-color-secondary); }
.visitor-online b { color: var(--el-color-success); }

/* 表格横向滚动：窄屏下 IP 列不溢出 */
.table-scroll {
  overflow-x: auto;
  -webkit-overflow-scrolling: touch;
}

/* ══════ 响应式：≤768px 移动端 ══════ */
@media (max-width: 768px) {
  /* 指标卡片单列全宽，数字缩小 */
  .stat-cards {
    gap: 8px;
  }

  .stat-card {
    flex: 1 1 100%;
  }

  .stat-value {
    font-size: 20px;
  }

  /* 图表降低高度，让两张图在竖屏下都可见 */
  .chart {
    height: 220px;
  }

  /* 图表头部（标题 + 切换按钮）纵向排布，避免按钮挤压 */
  .chart-head {
    flex-direction: column;
    align-items: flex-start;
    gap: 8px;
  }
}
</style>
