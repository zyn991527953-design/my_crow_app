#!/bin/bash
cd /app
echo "启动 Embedding 服务..."
python3 -m uvicorn embedding_service:app --host 0.0.0.0 --port 5003 --log-level info
