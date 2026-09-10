// K8s Dashboard - 替换原有的测试平台 Dashboard
(function() {
    const API_BASE = '/api';
    
    // 获取 K8s 数据
    async function fetchK8sData() {
        try {
            const [summary, pods, nodes] = await Promise.all([
                fetch(`${API_BASE}/summary`).then(r => r.json()),
                fetch(`${API_BASE}/pods`).then(r => r.json()),
                fetch(`${API_BASE}/nodes`).then(r => r.json())
            ]);
            return { summary, pods, nodes };
        } catch (err) {
            console.error('获取 K8s 数据失败:', err);
            return null;
        }
    }
    
    // 渲染统计卡片
    function renderStats(summary) {
        const statsData = [
            { title: '节点总数', value: summary.nodeCount || 0, color: '#1976d2' },
            { title: 'Pod 总数', value: summary.podCount || 0, color: '#388e3c' },
            { title: '运行中 Pod', value: summary.runningPods || 0, color: '#f57c00' },
            { title: '命名空间', value: summary.namespaceCount || 0, color: '#7b1fa2' }
        ];
        
        // 更新 DOM（需要根据实际页面结构调整选择器）
        const statElements = document.querySelectorAll('.stat-card .stat-number');
        statsData.forEach((stat, i) => {
            if (statElements[i]) statElements[i].textContent = stat.value;
        });
    }
    
    // 渲染图表（使用 ECharts）
    function renderCharts(pods, nodes) {
        // Pod 状态分布饼图
        const podStatus = { Running: 0, Pending: 0, Failed: 0, Succeeded: 0, Unknown: 0 };
        pods.forEach(p => { if (podStatus.hasOwnProperty(p.status)) podStatus[p.status]++; });
        
        // 节点状态
        const nodeStatus = { Ready: 0, NotReady: 0 };
        nodes.forEach(n => { nodeStatus[n.status]++; });
        
        // 渲染图表（需要在页面上有对应的容器）
        renderPieChart('pod-status-chart', podStatus);
        renderPieChart('node-status-chart', nodeStatus);
    }
    
    function renderPieChart(containerId, data) {
        const container = document.getElementById(containerId);
        if (!container || typeof echarts === 'undefined') return;
        
        const chart = echarts.init(container);
        chart.setOption({
            tooltip: { trigger: 'item' },
            legend: { bottom: '5%', left: 'center' },
            series: [{
                type: 'pie',
                radius: ['40%', '70%'],
                data: Object.entries(data).map(([name, value]) => ({ name, value }))
            }]
        });
    }
    
    // 渲染 Pod 表格
    function renderPodTable(pods) {
        const tbody = document.querySelector('#pod-table tbody');
        if (!tbody) return;
        
        tbody.innerHTML = pods.slice(0, 10).map(p => `
            <tr>
                <td>${p.name}</td>
                <td>${p.namespace}</td>
                <td><span class="status-badge status-${p.status.toLowerCase()}">${p.status}</span></td>
                <td>${p.node || '-'}</td>
                <td>${p.restarts || 0}</td>
            </tr>
        `).join('');
    }
    
    // 初始化
    window.addEventListener('load', async () => {
        const data = await fetchK8sData();
        if (data) {
            renderStats(data.summary);
            renderCharts(data.pods, data.nodes);
            renderPodTable(data.pods);
        }
        
        // 每 30 秒刷新
        setInterval(async () => {
            const newData = await fetchK8sData();
            if (newData) {
                renderStats(newData.summary);
                renderCharts(newData.pods, newData.nodes);
                renderPodTable(newData.pods);
            }
        }, 30000);
    });
})();
