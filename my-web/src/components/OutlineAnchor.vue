<!-- src/components/OutlineAnchor.vue 大纲锚点组件：接收锚点数据（外部传入），渲染 el-anchor 快速定位 -->
<template>
  <!-- 外层拦截点击：hash 路由下 <a href="#id"> 的默认跳转会改变 location.hash 被路由吃掉，
       导致页面空白、锚点失效，故 capture 阶段阻止默认行为并手动 scrollIntoView 定位 -->
  <div class="outline-anchor" @click.capture="handleClick">
    <!-- 无锚点时整组不渲染 -->
    <el-anchor v-if="tree.length" :offset="anchorOffset">
      <el-anchor-link v-for="item in tree" :key="item.id" :href="`#${item.id}`">
        {{ item.text }}
        <!-- 一级锚点下的子级标题 -->
        <template v-if="item.children && item.children.length" #sub-link>
          <el-anchor-link v-for="child in item.children" :key="child.id" :href="`#${child.id}`">
            {{ child.text }}
          </el-anchor-link>
        </template>
      </el-anchor-link>
    </el-anchor>
  </div>
</template>

<script lang="ts" setup>
import { computed, type PropType } from 'vue'
import type { AnchorItem } from './MdViewer.vue'

// 点击锚点链接：capture 阶段拦截默认跳转（hash 路由下 #id 会被 Vue Router 吞掉导致空白页），
// 改为手动滚动到对应标题；标题带 scroll-margin-top 避开吸顶导航
const handleClick = (e: MouseEvent) => {
  const link = (e.target as HTMLElement | null)?.closest?.('a[href^="#"]')
  if (!link) return
  const href = link.getAttribute('href') || ''
  const target = href ? document.querySelector(href) : null
  if (!target) return
  e.preventDefault()
  target.scrollIntoView({ behavior: 'smooth' })
}

// 树节点：一级锚点可携带子级
interface TreeNode extends AnchorItem {
  children: TreeNode[]
}

// props：anchors 为线性锚点列表（由 MdViewer 解析产生后传入）
const props = defineProps({
  anchors: {
    type: Array as PropType<AnchorItem[]>,
    default: () => []
  },
  // 锚点滚动偏移：避开吸顶导航栏
  anchorOffset: {
    type: Number,
    default: 70
  }
})

// 线性锚点 -> 层级树：h1/h2 作为一级，h3 及以下挂到最近一个一级下
const tree = computed<TreeNode[]>(() => {
  const result: TreeNode[] = []
  let lastTop: TreeNode | null = null

  for (const item of props.anchors) {
    if (item.level <= 2) {
      lastTop = { ...item, children: [] }
      result.push(lastTop)
    } else if (lastTop) {
      lastTop.children.push({ ...item, children: [] })
    }
    // h3 之前没有 h1/h2 时忽略（文档通常以一级标题开头）
  }
  return result
})
</script>
