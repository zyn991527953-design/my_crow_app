#!/bin/bash
#
# 复制文件到前端容器
docker cp ~/my_crow_app/index.html frontend:/usr/share/nginx/html/
docker cp k8s-login.html frontend:/usr/share/nginx/html/login.html

# 修改 index.html，添加登录检查
docker exec frontend sh -c "
sed -i 's|<script>|<script>\nif(!localStorage.getItem(\"token\")){window.location.href=\"/login.html\";}|' /usr/share/nginx/html/index.html
"

docker restart frontend
