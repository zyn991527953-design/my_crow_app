cat > /etc/systemd/system/kubelet.service.d/20-swap.conf << EOF
[Service]
Environment="KUBELET_EXTRA_ARGS=--fail-swap-on=false"
EOF

# 2. 重新加载并重启
systemctl daemon-reload
systemctl restart kubelet

# 3. 查看状态
systemctl status kubelet
