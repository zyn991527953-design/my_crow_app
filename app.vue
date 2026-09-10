<!-- App.vue -->
<template>
  <div class="dashboard">
    <h1>K8s 集群管理平台</h1>
    
    <!-- 概览卡片 -->
    <div class="summary-cards">
      <div class="card">
        <h3>节点数</h3>
        <p>{{ summary.nodeCount || 0 }}</p>
      </div>
      <div class="card">
        <h3>Pod 总数</h3>
        <p>{{ summary.podCount || 0 }}</p>
      </div>
      <div class="card running">
        <h3>运行中</h3>
        <p>{{ summary.runningPods || 0 }}</p>
      </div>
      <div class="card pending">
        <h3>等待中</h3>
        <p>{{ summary.pendingPods || 0 }}</p>
      </div>
    </div>

    <!-- 节点列表 -->
    <h2>节点列表</h2>
    <table>
      <thead>
        <tr><th>名称</th><th>状态</th><th>Kubelet 版本</th><th>操作系统</th></tr>
      </thead>
      <tbody>
        <tr v-for="node in nodes" :key="node.name">
          <td>{{ node.name }}</td>
          <td :class="node.status === 'Ready' ? 'ready' : 'not-ready'">{{ node.status }}</td>
          <td>{{ node.kubelet }}</td>
          <td>{{ node.os }}</td>
        </tr>
      </tbody>
    </table>

    <!-- Pods 列表 -->
    <h2>Pods 列表</h2>
    <table>
      <thead>
        <tr><th>名称</th><th>命名空间</th><th>状态</th><th>IP</th><th>节点</th><th>操作</th></tr>
      </thead>
      <tbody>
        <tr v-for="pod in pods" :key="pod.name">
          <td>{{ pod.name }}</td>
          <td>{{ pod.namespace }}</td>
          <td :class="pod.status === 'Running' ? 'running' : 'other'">{{ pod.status }}</td>
          <td>{{ pod.ip || '-' }}</td>
          <td>{{ pod.node || '-' }}</td>
          <td><button @click="deletePod(pod.namespace, pod.name)">删除</button></td>
        </tr>
      </tbody>
    </table>

    <h2>Deployments</h2>
    <table>
      <thead>
        <tr><th>名称</th><th>命名空间</th><th>期望副本</th><th>可用副本</th></tr>
      </thead>
      <tbody>
        <tr v-for="deploy in deployments" :key="deploy.name">
          <td>{{ deploy.name }}</td>
          <td>{{ deploy.namespace }}</td>
          <td>{{ deploy.replicas }}</td>
          <td>{{ deploy.available || 0 }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script>
import axios from 'axios'

const API_BASE = '/api'

export default {
  data() {
    return {
      summary: {},
      nodes: [],
      pods: [],
      deployments: []
    }
  },
  mounted() {
    this.fetchAll()
    // 每 10 秒自动刷新
    setInterval(() => this.fetchAll(), 10000)
  },
  methods: {
    async fetchAll() {
      try {
        const [summary, nodes, pods, deployments] = await Promise.all([
          axios.get(`${API_BASE}/summary`),
          axios.get(`${API_BASE}/nodes`),
          axios.get(`${API_BASE}/pods`),
          axios.get(`${API_BASE}/deployments`)
        ])
        this.summary = summary.data
        this.nodes = nodes.data
        this.pods = pods.data
        this.deployments = deployments.data
      } catch (err) {
        console.error('获取数据失败:', err)
      }
    },
    async deletePod(namespace, name) {
      if (!confirm(`确定要删除 Pod ${name} 吗？`)) return
      try {
        await axios.delete(`${API_BASE}/namespaces/${namespace}/pods/${name}`)
        this.fetchAll()
      } catch (err) {
        alert('删除失败: ' + err.message)
      }
    }
  }
}
</script>

<style>
.dashboard { padding: 20px; font-family: sans-serif; }
.summary-cards { display: flex; gap: 20px; margin-bottom: 30px; }
.card { padding: 20px; border-radius: 8px; background: #f5f5f5; min-width: 120px; }
.card h3 { margin: 0 0 10px; }
.card p { font-size: 28px; margin: 0; font-weight: bold; }
.ready, .running { color: green; }
.not-ready { color: red; }
.other { color: orange; }
table { border-collapse: collapse; width: 100%; margin-bottom: 30px; }
th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }
th { background: #f2f2f2; }
button { background: #dc3545; color: white; border: none; padding: 5px 10px; border-radius: 4px; cursor: pointer; }
</style>
