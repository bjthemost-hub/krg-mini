#!/bin/bash
# install-krg51.sh <node_id>   -- run as root from this directory.
# Installs the 5.1 build, the krg modules, the tools and the units, then makes
# GRUB default to it. Does not reboot.
set -e
SRC=$(cd "$(dirname "$0")" && pwd)
NID="${1:?usage: install-krg51.sh <node_id>}"

install -D -m 0644 "$SRC/boot/vmlinuz-5.1.0-krg" /boot/vmlinuz-5.1.0-krg
mkdir -p /lib/modules/5.1.0/kernel
install -m 0644 "$SRC/lib/modules/5.1.0/kernel/krg_taskview.ko" /lib/modules/5.1.0/kernel/
install -m 0644 "$SRC/lib/modules/5.1.0/kernel/krg_dsm.ko"      /lib/modules/5.1.0/kernel/

for f in krgtool krgsys krg-crack krgsh krg-pi krg-top krgdsm krgreport.sh; do
  install -m 0755 "$SRC/usr/local/bin/$f" /usr/local/bin/$f
done
for u in krgcheck krgserve krgreport; do
  install -m 0644 "$SRC/etc/systemd/system/$u.service" /etc/systemd/system/$u.service
done

echo "$NID" > /etc/krg-node-id

# Root UUID. In a chroot (autoinstall late-commands) `findmnt /` still reports the
# *live* root, so take the root line out of this system's own /etc/fstab first.
ROOTUUID="${KRG_ROOT_UUID:-}"
if [ -z "$ROOTUUID" ] && [ -r /etc/fstab ]; then
  ROOTDEV=$(awk '$2=="/"{print $1; exit}' /etc/fstab)
  case "$ROOTDEV" in
    UUID=*)                ROOTUUID=${ROOTDEV#UUID=} ;;
    /dev/disk/by-uuid/*)   ROOTUUID=${ROOTDEV##*/} ;;
    /dev/*)                ROOTUUID=$(blkid -s UUID -o value "$ROOTDEV" 2>/dev/null || true) ;;
  esac
fi
[ -n "$ROOTUUID" ] || ROOTUUID=$(findmnt -no UUID -T / 2>/dev/null || true)
if [ -z "$ROOTUUID" ]; then
  echo "KRG51: cannot determine root UUID; re-run with KRG_ROOT_UUID=<uuid>" >&2
  exit 1
fi
if command -v blkid >/dev/null 2>&1 && [ -z "$(blkid -U "$ROOTUUID" 2>/dev/null)" ]; then
  echo "KRG51: warning: no block device carries UUID $ROOTUUID (GRUB will not find the root)" >&2
fi

cat > /etc/grub.d/40_krg51 <<EOF
#!/bin/sh
exec tail -n +3 \$0
menuentry 'Khz 5.1 (our build, no initrd)' --class ubuntu --class gnu-linux {
	insmod part_gpt
	insmod part_msdos
	insmod ext2
	search --no-floppy --fs-uuid --set=root $ROOTUUID
	linux /boot/vmlinuz-5.1.0-krg root=UUID=$ROOTUUID ro console=tty0 console=ttyS0,115200
}
EOF
chmod +x /etc/grub.d/40_krg51
update-grub

systemctl daemon-reload
systemctl enable krgcheck.service krgserve.service krgreport.service
echo "KRG51_OK node=$NID rootuuid=$ROOTUUID"

# --- repo note -------------------------------------------------------------
# This is the installer shipped inside krg51-kernel-*.tar.gz.
# Fixed 2026-10-28: the GRUB entry used to take its root UUID from findmnt /,
# which reports the *live* environment when the script runs inside a chroot
# (autoinstall late-commands), so the freshly installed box panicked with
#   VFS: Cannot open root device "UUID=<live env uuid>" ... unknown-block(0,0)
# It now reads the root line out of the target's own /etc/fstab first, and
# falls back to findmnt only when there is no usable fstab entry.
# KRG_ROOT_UUID=<uuid> overrides everything.
# ---------------------------------------------------------------------------