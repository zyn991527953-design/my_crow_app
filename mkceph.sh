# SSH 到 worker-7
ssh k8s-workers-7

# 创建 systemd service 文件
sudo tee /etc/systemd/system/cephfs-mount.service << 'EOF'
[Unit]
Description=CephFS Mount
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/ceph-fuse -f /mnt/cephfs --id admin --keyring /etc/ceph/ceph.client.admin.keyring --conf=/etc/ceph/ceph.conf
Restart=always
RestartSec=10
KillMode=mixed
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
EOF

# 创建挂载点
sudo mkdir -p /mnt/cephfs

# 重新加载 systemd
sudo systemctl daemon-reload

# 启用并启动服务
sudo systemctl enable cephfs-mount
sudo systemctl start cephfs-mount

# 检查状态
sudo systemctl status cephfs-mount

# 查看挂载是否成功
df -h | grep cephfs
