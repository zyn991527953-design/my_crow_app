sudo pkill -9 ceph-fuse

# 2. 强制卸载
sudo umount -l /mnt/cephfs 2>/dev/null
sudo umount -f /mnt/cephfs 2>/dev/null

# 3. 使用 fusermount 清理
fusermount -uz /mnt/cephfs 2>/dev/null

# 4. 检查是否还有挂载
cat /proc/mounts | grep cephfs
