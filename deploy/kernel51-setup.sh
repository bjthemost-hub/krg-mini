#!/bin/bash
# Drop the 5.1 kernel build + krg_taskview into an *existing* Ubuntu 22.04 VM
# and switch it to boot that kernel. Nothing here reboots the box.
#
#   KRG_PKG_URL=http://<pkg-host>:8899 ./kernel51-setup.sh <node_id>
#
# The package host must serve these files:
#   vmlinuz-5.1.0-krg  krg_taskview.ko  krgtool  krgsys
#   krgcheck.service   krgserve.service  krgreport.service  krgreport.sh
#   nodes.list         (one "<hostname> <host:port>" line per node)
set -e

URL="${KRG_PKG_URL:?set KRG_PKG_URL, e.g. http://10.0.0.1:8899}"
NID="$1"
[ -z "$NID" ] && { echo "usage: KRG_PKG_URL=... $0 <node_id>"; exit 1; }

wget -q -O /boot/vmlinuz-5.1.0-krg "$URL/vmlinuz-5.1.0-krg"
mkdir -p /lib/modules/5.1.0/kernel
wget -q -O /lib/modules/5.1.0/kernel/krg_taskview.ko "$URL/krg_taskview.ko"

# The old user-space agent may be running krgtool; overwriting it in place
# fails with ETXTBSY, so stage to .new and rename.
wget -q -O /usr/local/bin/krgtool.new "$URL/krgtool"
wget -q -O /usr/local/bin/krgsys.new  "$URL/krgsys"
wget -q -O /usr/local/bin/krgreport.sh.new "$URL/krgreport.sh"
chmod +x /usr/local/bin/krgtool.new /usr/local/bin/krgsys.new /usr/local/bin/krgreport.sh.new
mv -f /usr/local/bin/krgtool.new /usr/local/bin/krgtool
mv -f /usr/local/bin/krgsys.new  /usr/local/bin/krgsys
mv -f /usr/local/bin/krgreport.sh.new /usr/local/bin/krgreport.sh

echo "$NID" > /etc/krg-node-id
wget -q -O /etc/krg-nodes.list "$URL/nodes.list"
awk -v h="$(hostname)" '$1!=h {printf "%s,", $2}' /etc/krg-nodes.list | sed 's/,$//' > /etc/krg-peers

wget -q -O /etc/systemd/system/krgcheck.service    "$URL/krgcheck.service"
wget -q -O /etc/systemd/system/krgserve.service    "$URL/krgserve.service"
wget -q -O /etc/systemd/system/krgreport.service   "$URL/krgreport.service"

# Boot the 5.1 build by default. This appends to grub.cfg directly, so a later
# update-grub wipes it -- move it into /etc/grub.d/40_krg51 for a permanent box.
if ! grep -q 'vmlinuz-5.1.0-krg' /boot/grub/grub.cfg; then
cat >> /boot/grub/grub.cfg <<'EOF'

menuentry 'Khz 5.1 (our build, no initrd)' --class ubuntu --class gnu-linux {
	insmod part_msdos
	insmod ext2
	search --no-floppy --label --set=root cloudimg-rootfs
	linux /boot/vmlinuz-5.1.0-krg root=/dev/sda1 ro console=tty0 console=ttyS0,115200
}
set default="Khz 5.1 (our build, no initrd)"
set timeout=5
EOF
fi

# The in-kernel exporter replaces the user-space one.
systemctl disable --now krg-mini.service 2>/dev/null || true
systemctl daemon-reload
systemctl enable krgcheck.service krgserve.service krgreport.service 2>&1 | tail -3

echo "SETUP_OK node=$NID host=$(hostname) peers=$(tr ',' ' ' < /etc/krg-peers | wc -w)"