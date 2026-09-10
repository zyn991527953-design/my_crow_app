#!/bin/bash
echo "🔨 C++ 开始编译（宿主机）..."

# 宿主机编译
g++ -std=c++17 -O2 \
    -I./Crow/include \
    -o crow-k8s-api main.cpp \
    -lcurl -lpthread -lldap -lpqxx -lpq -lhiredis -lrados -lsqlite3

# 检查编译是否成功
if [ $? -eq 0 ]; then
    echo "✅ C++ 编译完成"
else
    echo "❌ 编译失败，退出"
    exit 1
fi

echo "🔨 构建 Docker 镜像..."
docker build -t cpp-backend .

# 停止并删除旧容器
docker stop backend 2>/dev/null
docker rm backend 2>/dev/null

# 启动容器
docker run -d \
  --name backend \
  --network test-platform \
  -p 5005:5005 \
  -p 8082:8082 \
  -p 8000:8580 \
  -p 5001:5001 \
  -p 5003:5003 \
  -v /mnt/cephfs:/mnt/cephfs \
  -v /etc/ceph:/etc/ceph:ro \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -v /usr/bin/docker:/usr/bin/docker \
  -v $(pwd)/models/paraphrase-multilingual-MiniLM-L12-v2:/app/models/paraphrase-multilingual-MiniLM-L12-v2 \
  -v $(pwd)/chain_data:/root \
  -e K8S_TOKEN="eyJhbGciOiJSUzI1NiIsImtpZCI6IllZY2M0em5fa2lrb0IwVGVhWkRILWN6RVA0c1pBeTNkcWJhVUFRbEVYNEUifQ.eyJpc3MiOiJrdWJlcm5ldGVzL3NlcnZpY2VhY2NvdW50Iiwia3ViZXJuZXRlcy5pby9zZXJ2aWNlYWNjb3VudC9uYW1lc3BhY2UiOiJkZWZhdWx0Iiwia3ViZXJuZXRlcy5pby9zZXJ2aWNlYWNjb3VudC9zZWNyZXQubmFtZSI6ImNyb3ctYXBpLXRva2VuIiwia3ViZXJuZXRlcy5pby9zZXJ2aWNlYWNjb3VudC9zZXJ2aWNlLWFjY291bnQubmFtZSI6ImNyb3ctYXBpIiwia3ViZXJuZXRlcy5pby9zZXJ2aWNlYWNjb3VudC9zZXJ2aWNlLWFjY291bnQudWlkIjoiYmQyOTkxNjYtNTkwOC00ZjUwLWFkMTAtOWMzOGM1NjZiZjNhIiwic3ViIjoic3lzdGVtOnNlcnZpY2VhY2NvdW50OmRlZmF1bHQ6Y3Jvdy1hcGkifQ.FxAs0clJYVB7EaxrAg6oHJJDInog2YIaY2GnUCGF7RuqQXoLv2vvCV12PWK9rrTkiVLa8PpT6osHP-q4IXSqp-I5wdWpORO0bxn-fVkx8L6tVYJGTIKsFCqPQqyhlA6viBA8lxj-gTa2kloxqs7W8gdyG04WKh0Aq9XsJqmbGRzOVaG5T07nXdWTm2x8kmDy8IArLRu3QbHuNur4LyjzBlXIqM39jWDTxJK2Nt-JDPkKQYYRgJO5wZx2iY6FDPHKCWQ2FuvWBbQu1bliuYP9JKBJk93D9TUdlnzB2SFX6FoePRclmfFB-U9ApD7fehis18S4u0-JaaNaazULI4iwyg" \
  -e K8S_API_SERVER="https://192.168.138.139:6443" \
  cpp-backend

# 复制编译好的二进制到容器
echo "📦 复制二进制文件到容器..."
docker cp crow-k8s-api backend:/app/crow-k8s-api

# 重启容器
docker restart backend

echo "✅ 容器已启动并更新"
docker logs -f backend
