#!/bin/sh
# run as root on every cluster node
export DEBIAN_FRONTEND=noninteractive
if ! dpkg -s nfs-common >/dev/null 2>&1; then
	apt-get install -y -qq nfs-common >/tmp/nfsapt.log 2>&1 || { echo "APT_FAIL"; tail -2 /tmp/nfsapt.log; exit 1; }
fi
grep -q ' /krg ' /etc/fstab 2>/dev/null || echo '192.168.6.13:/srv/krg /krg nfs vers=4.0,hard,intr,noatime 0 0' >> /etc/fstab
mkdir -p /krg
mountpoint -q /krg || mount -a 2>/tmp/mnt.log
if mountpoint -q /krg; then
	echo "OK $(hostname) $(id -u) inode41=$(stat -c %i /krg/from41.txt 2>/dev/null)"
else
	echo "MOUNT_FAIL $(hostname)"; cat /tmp/mnt.log
fi
