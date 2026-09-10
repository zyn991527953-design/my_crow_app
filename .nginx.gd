server {
    listen 80;
    server_name localhost;
    root /usr/share/nginx/html;
    index login.html;

    location / {
        try_files $uri $uri/ /index.html;
    }

    location /api/ {
        proxy_pass http://backend:8580/api/;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    location /grafana {
        proxy_pass http://192.168.138.139:30030;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_hide_header X-Frame-Options;
        proxy_redirect http://192.168.138.139:30030/ /grafana/;
    }

    # 🔑 Jenkins 代理（精准处理登录重定向）
    location /jenkins/ {
        proxy_pass http://192.168.138.139:31730;
        
        # 传递完整客户端信息（让 Jenkins 知道真实请求来源）
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-Forwarded-Host $host;
        proxy_set_header X-Forwarded-Port $server_port;
        
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        
        # 🔥 关键：拦截所有可能的重定向目标，统一映射到前端路径
        proxy_redirect / /jenkins/;
        proxy_redirect http://192.168.138.139:31730/ /jenkins/;
        proxy_redirect http://$host:31730/ /jenkins/;
        
        # 隐藏安全头
        proxy_hide_header Content-Security-Policy;
        proxy_hide_header Content-Security-Policy-Report-Only;
        proxy_hide_header X-Frame-Options;
        
        proxy_connect_timeout 60s;
        proxy_read_timeout 300s;
        proxy_send_timeout 300s;
    }
}
