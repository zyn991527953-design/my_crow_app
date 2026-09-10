#!/bin/bash

# 启动 Terminal Server（使用 -port 参数）
/usr/local/bin/webshell -port 8082 &

# 设置 HuggingFace 镜像（防止需要联网下载）
export HF_ENDPOINT=https://hf-mirror.com

# 启动 Embedding 服务
echo "启动 Embedding 服务..."
cd /app
python3 -m uvicorn embedding_service:app --host 0.0.0.0 --port 5003 --log-level info &

# 等待 Embedding 服务启动
sleep 15

# 启动 Flask Ceph 服务
cd /app/flask-ceph-upload
nohup python3 app.py > /var/log/flask-ceph.log 2>&1 &


# ========== 启动 PytestWEB 服务 ==========
echo "启动 PytestWEB 服务..."
cd /root/sdc-integration-tests/ecosda/ecosdaapp

# 检查是否有 app.py
if [ -f app.py ]; then
    # 先杀掉可能占用 5005 端口的进程
    pkill -f "ecosdaapp/app.py" 2>/dev/null
    sleep 2
    # 启动 PytestWEB
    nohup python3 app.py --host=0.0.0.0 --port=5005 > /var/log/pytestweb.log 2>&1 &
    echo "✅ PytestWEB 已启动 (端口 5005)"
else
    echo "❌ app.py 不存在: /root/sdc-integration-tests/ecosda/ecosdaapp/"
fi &

# 等待 PytestWEB 启动
sleep 5

# 启动 C++ 后端
cd /app
./crow-k8s-api
