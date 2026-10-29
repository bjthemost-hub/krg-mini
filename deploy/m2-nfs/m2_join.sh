#!/bin/sh
export DEBIAN_FRONTEND=noninteractive
echo "=== nfs-common ==="
dpkg -s nfs-common 2>/dev/null | grep -E '^Status' || apt-get install -y -qq nfs-common 2>&1 | tail -2
echo "=== mount ==="
mkdir -p /krg
mountpoint -q /krg || mount -t nfs -o vers=4.0 192.168.6.13:/srv/krg /krg
mountpoint /krg
df -h /krg | tail -1
echo "=== read what node 41 wrote ==="
cat /krg/from41.txt
echo "=== write ours ==="
echo "hello-from-$(hostname) $(date -Is)" > /krg/from24.txt
ls -l /krg
