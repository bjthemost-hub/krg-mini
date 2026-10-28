#!/bin/bash
# demo.sh cpu | mem-home | mem-reader      -- khz v0.1 现场演示
set -u
NODES=/etc/krg-nodes.list
PEERS=$(awk '{print $2}' $NODES | paste -sd,)

case "${1:-}" in
cpu)
	echo "[A] 每台机器自己报的核数（krgsh 把命令发到每个节点各跑一遍）"
	krgsh -c 'bash nproc' 2>/dev/null | head -30
	echo
	echo "[B] 合并后的进程表：一个 ps 看到全集群（只显示前 8 行）"
	krgtool show "$PEERS" 2>/dev/null | head -8
	echo
	echo "[C] 新任务由内核挑节点：不给目标，看它落在哪、留了什么"
	krgtool spawn --auto --nodes $NODES "sleep 30" 2>&1 | tail -3
	krgtool state 2>&1 | head -5
	;;
mem-home)
	krgdsm free pool >/dev/null 2>&1
	echo "[A] 只登记槽位，不占页内存"
	krgdsm alloc pool 65536
	krgdsm info pool
	free -m | sed -n '2p'
	echo
	echo "[B] 写满 65536 页 = 256 MiB"
	krgdsm fillrange pool 0 65536
	krgdsm seal pool
	krgdsm info pool
	free -m | sed -n '2p'
	;;
mem-reader)
	PEER=10.0.2.2:7801
	echo "[A] 读端只给 64 页缓存（256 KiB），把 256 MiB 读一遍"
	krgdsm free pool >/dev/null 2>&1
	krgdsm alloc pool 65536 --cache 64
	free -m | sed -n '2p'
	krgdsm stream pool 0 65535 $PEER --verify
	krgdsm info pool
	free -m | sed -n '2p'
	echo
	echo "[B] 同一段当成内存用：mmap + 指针，一次 read() 都没有"
	krgdsm free pool >/dev/null 2>&1
	krgdsm alloc pool 65536
	dsmread pool $PEER
	free -m | sed -n '2p'
	;;
*)
	echo "usage: $0 cpu|mem-home|mem-reader"
	exit 2
	;;
esac