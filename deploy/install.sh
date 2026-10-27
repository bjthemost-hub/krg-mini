#!/bin/bash
# install.sh <node_id> [nodes.list]
#
#   node_id     1..255, unique per machine; becomes the high byte of every gpid
#               (gpid = node_id << 24 | pid) and must not be reused.
#   nodes.list  optional; installed as /etc/krg-nodes.list (cluster members).
#
# Builds the kernel module and userspace tools, installs them, and (re)starts
# krg-mini.service. Re-running is safe: it replaces the module and restarts.
set -e
NODE_ID=${1:?usage: install.sh <node_id> [nodes.list]}
HERE=$(cd "$(dirname "$0")/.." && pwd)

echo "options krg_taskview node_id=$NODE_ID" > /etc/modprobe.d/krg-node.conf

make -C "$HERE/module"
install -D "$HERE/module/krg_taskview.ko" \
        "/lib/modules/$(uname -r)/extra/krg_taskview.ko"
install -D "$HERE/module/krg_dsm.ko" \
        "/lib/modules/$(uname -r)/extra/krg_dsm.ko"
depmod -a

make -C "$HERE/user"
install -m 0755 "$HERE/user/krgtool"  /usr/local/bin/krgtool
install -m 0755 "$HERE/user/krg-pi.py" /usr/local/bin/krg-pi
install -m 0755 "$HERE/user/krgsh.py"  /usr/local/bin/krgsh
install -m 0755 "$HERE/user/krg-crack.py" /usr/local/bin/krg-crack
install -m 0755 "$HERE/user/krg-top.py" /usr/local/bin/krg-top
install -m 0755 "$HERE/user/krgdsm.py" /usr/local/bin/krgdsm
install -m 0755 "$HERE/user/dsmread"   /usr/local/bin/dsmread
install -m 0644 "$HERE/deploy/krg-mini.service" /etc/systemd/system/krg-mini.service

if [ -n "${2:-}" ]; then
  install -m 0644 "$2" /etc/krg-nodes.list
fi

pkill -f "krgtool serve" 2>/dev/null || true
rmmod krg_taskview 2>/dev/null || true
rmmod krg_dsm 2>/dev/null || true
systemctl daemon-reload
systemctl enable --now krg-mini.service
sleep 2

systemctl is-enabled krg-mini
systemctl is-active krg-mini
modinfo -F name "/lib/modules/$(uname -r)/extra/krg_taskview.ko"
ss -ltn | grep -q ":7700 " && echo "krgtool serve listening on 7700"
