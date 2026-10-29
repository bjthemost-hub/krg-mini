#!/bin/sh
set -e
mkdir -p /srv/krg
chmod 755 /srv/krg
if ! grep -q '^/srv/krg' /etc/exports 2>/dev/null; then
	echo '/srv/krg 192.168.6.0/24(rw,sync,no_subtree_check,no_root_squash)' >> /etc/exports
fi
exportfs -ra
systemctl restart nfs-server
sleep 2
echo "=== exports ==="
exportfs -v
echo "=== active ==="
systemctl is-active nfs-server
echo "=== listening 2049 ==="
ss -lntp | grep 2049 || true
