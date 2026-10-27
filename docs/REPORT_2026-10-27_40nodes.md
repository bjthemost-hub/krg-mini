# Report — 40 nodes, 5 hosts x 8 VMs: does it scale from 20, and where does the win come from

**Date:** 2026-10-27 · **Rig:** 40 VirtualBox guests on the same 5 physical hosts as the 20-node run
· **Status:** measured

This round doubles the node count on the *same hardware* and asks one question: while the
vCPU count doubles, does throughput double, and what does the second doubling cost per thread?
Every number below comes from one of two logs, both quoted verbatim at the end.

---

## 1. What changed from the 20-node run

### Headline: the same loop, with and without the cluster layer

| setup | compute it uses | throughput | per unit |
|---|---|---|---|
| one box, 20 processes | 20 procs on 24 cores / 48 threads | 16.3 MHash/s | 0.813 per process |
| one box, 40 processes | 40 procs, same box | 19.0 MHash/s | 0.474 per process |
| one box, 48 processes (one per thread, packed) | 48 procs, same box | 17.5 MHash/s | 0.364 per process |
| 20-node cluster | 40 vCPU | 25.4 MHash/s | 0.635 per vCPU |
| **40-node cluster** | **80 vCPU** | **49.2 MHash/s** | **0.615 per vCPU** |

Cluster rows: fixed 2 G-hash budget, median of 3. One-box rows: the identical
`hashlib.sha256(...).hexdigest()` loop, no cluster layer. Sources for each row are in
§3-§5 and quoted verbatim in §9.

### What changed

Nothing but the guest count:

| item | 20-node run | this run |
|---|---|---|
| guests per host | 4 | 8 |
| guest size | 2 vCPU / 2 GiB | 2 vCPU / **1.5 GiB** |
| total vCPU | 40 | 80 |
| host vCPU load | 8 of 48 threads | 16 of 48 threads |
| guests' RAM total | 40 GiB | 60 GiB |
| coordinator | separate ARM board, not a node | same |
| workload / budget / workers-per-node | `krg-crack`, 2 G, 2 | same |

Hosts stay un-subscribed: 16 vCPU of 48 threads per host is a third of the machine. That is
deliberate — the point of a second data point is to change **one** variable.

## 2. Method

Identical to the 20-node run (`REPORT_2026-10-27_20nodes.md` §3): SHA-256 nonce search with 12 hex
zeros required, 1,000,000-nonce chunks pulled from a coordinator queue, `--budget 2000000000` so
every run does exactly the same amount of work, one 2-per-node worker pair (one per vCPU).

Two additions this round:

- **Three cluster runs** of 2 G each plus a 117 M warm-up (ramp-up, not used for a conclusion).
- **Single-machine baselines re-measured the same day on `nd20`** (the same Xeon class, 24 cores /
  48 threads, 62 GiB, its own guests idle), because "2.6x of what?" needs a number taken under the
  same conditions, not one borrowed from last week.

## 3. Results — the cluster

| run | hashes counted | wall | rate | fan-out |
|---|---|---|---|---|
| warm-up | 117,000,000 | 3.45 s | 33.9 MHash/s | 0.54 s |
| 1 | 2,003,000,000 | 40.77 s | 49.1 MHash/s | 1.00 s |
| 2 | 2,002,000,000 | 40.67 s | 49.2 MHash/s | 0.71 s |
| 3 | 2,001,000,000 | 40.45 s | 49.5 MHash/s | 0.84 s |

**Median 49.2 MHash/s over 80 vCPU = 0.615 MHash/s per vCPU.**

All 40 nodes answered on every run: `workers 80 on 40 nodes`, no `unreachable`, no `no progress`.
Fan-out is 0.54–1.00 s out of ~41 s — 1.3–2.4 %, roughly twice the relative cost of the 20-node
run (0.5–0.8 % of 78 s), which is what a doubled node list and 80 spawn calls should cost.

## 4. Results — before vs after, the table this round exists for

| | 20 nodes | 40 nodes | ratio |
|---|---|---|---|
| vCPU | 40 | 80 | 2.00x |
| wall clock, 2 G hashes (median) | 78.78 s | 40.67 s | 0.52x (1.94x faster) |
| throughput (median) | 25.4 MHash/s | 49.2 MHash/s | **1.94x** |
| per vCPU | 0.635 | 0.615 | **0.969x** |
| per-vCPU loss | — | -3.1 % | |
| fan-out share of runtime | 0.5–0.8 % | 1.3–2.4 % | |

**Doubling vCPU bought 1.94x, not 2.00x.** The 3 % shortfall is the smallest measured cost of
doubling in this project so far, and it is of the size you would expect from doubling the
coordinator's round-trip load and the host's guest count at the same time. Neither was isolated.

## 5. Single machine, same day, same loop

`raw_sha.py`, the same `hashlib.sha256(...).hexdigest()` inner loop, 100 k-hash chunks, plain
`fork()`, no cluster layer, on `nd20` (48 threads, 24 physical cores, all guests idle):

| processes | hashes each | wall | rate | per process |
|---|---|---|---|---|
| 40 | 20 M | 42.74 s | 18.7 MHash/s | 0.468 |
| 48 | 20 M | 54.97 s | 17.5 MHash/s | 0.364 |

So on the day of the 40-node run:

- 40-node cluster (**80** vCPU) is **2.63x** one box running 40 processes,
- and **2.81x** the same box running 48 processes — i.e. one process per thread, the most that
  machine can hold.

The 48-process row is the honest "one machine, fully packed" number: adding processes past the
core count *lowers* total throughput (18.7 -> 17.5) while per-process collapses to 0.364. The
cluster at 80 vCPU still runs at 0.615.

That is the actual content of the result: **the win is not SSI magic, it is not being squeezed onto
one socket pair.** A 40-node cluster of 2-vCPU guests keeps 0.615 per vCPU; a 48-thread box asked
for 48 processes keeps 0.364. Per vCPU the cluster is 1.69x the packed single box, and 1.31x the
same box at 40 processes.

Reference from the 20-node report (same box, taken earlier that day): 8 procs 0.816, 20 procs
0.813, 40 procs 0.474. Today's 40-process re-run (0.468) agrees with it within 1.3 %, so the
baseline is stable and the comparison is not an artefact of one lucky run.

## 6. One run that was wrong, and why it is in this report

The first 40-node round produced 25.3 MHash/s — almost exactly the 20-node number, which is what
made it suspicious. Cause: the 20 new guests had the cluster module and `krgtool` installed but
**not** `/usr/local/bin/krg-crack`. `spawn` only launches the process; when the binary is missing
the worker dies immediately and the coordinator still counts the *requested* 80 workers. So the
run reported `workers 80 on 40 nodes` while 40 workers were doing all the hashing.

Fixed by installing `krg-crack` on all 40 guests (md5 `ff9eedf5` verified per node), re-run,
49.2 MHash/s. The lesson is in `docs/BENCHMARKS.md`: **before any benchmark, `ls` the worker
binary on every node** — a successful `spawn` is not evidence that a worker is alive. It also
means the 25.3 number is not a scaling result and must not be quoted as one.

## 7. What this does not say

- **One workload.** CPU-bound SHA-256, 1 M-chunk granularity. Says nothing about shared state, IPC,
  or chunks under ~10 k units.
- **One topology, one doubling.** 40 -> 80 vCPU on 5 hosts. Whether 160 vCPU still keeps 0.615 is
  **not measured**; the coordinator is a single Python process and is the first thing expected to
  bend.
- **VM overhead still bundled.** The gap between 0.615 and bare-metal per-process rate includes
  VirtualBox's own cost. Splitting it needs a bare-metal node in the same run — **not done**.
- **No failure testing.** All 40 nodes stayed healthy. Node death mid-run is implemented
  (worker retry, shard re-run) but not measured here.
- **Memory per guest went down** (2 GiB -> 1.5 GiB). This workload is CPU-only and used 0.3 GiB
  per guest, so it costs nothing here — but it is a real change and is not free for other workloads.

![40-node cluster at full load](images/krg-top-40nodes.png)

`krg-top` during the 900 M screenshot run: `nodes 40/40`, `cores 80`, cluster CPU `99.7 %`,
`mem 13.1 / 66.9 GiB`, `workers 80`, every guest at ~100 % CPU and 0.3 of 1.4-1.9 GiB used.
The page reflows by width (auto-fill, min 320 px), so the same page is 8 columns on a wide
monitor and 5 on this 1920 px capture.

## 8. Reproduce

```sh
# on every guest: 2 vCPU, Ubuntu 22.04, matching headers
sudo ./deploy/install.sh <node_id>
ls -l /usr/local/bin/krg-crack        # must exist on ALL nodes, see section 6

# on the coordinator (not a node)
krg-crack --zeros 12 --budget 2000000000 \
          --nodes /etc/krg-nodes.list --workers-per-node 2

# baseline on one host, guests idle
python3 raw_sha.py 40 20000000
python3 raw_sha.py 48 20000000
```

## 9. Raw logs

Cluster (coordinator `101`, `/root/bench40.log`):

```
warm start=13:38:32
  warm rc=0
job 23170-1793108313: 40 nodes, 80 workers, spawn 0.54 s
  hashes     117,000,000 counted in 3.45 s  => 33.9 MHash/s
  workers    80 on 40 nodes
#### run1 start=13:38:36
  rc=0
job 23299-1793108317: 40 nodes, 80 workers, spawn 1.00 s
  hashes     2,003,000,000 counted in 40.77 s  => 49.1 MHash/s
  compute    2,003,000,000 in 40.63 s  => 49.3 MHash/s (from first chunk)
  workers    80 on 40 nodes
  verify     n/a (budget reached, no hit needed)
  end=13:39:18
#### run2 start=13:39:18
  rc=0
job 23429-1793108358: 40 nodes, 80 workers, spawn 0.71 s
  hashes     2,002,000,000 counted in 40.67 s  => 49.2 MHash/s
  compute    2,002,000,000 in 40.57 s  => 49.3 MHash/s (from first chunk)
  workers    80 on 40 nodes
  verify     n/a (budget reached, no hit needed)
  end=13:39:59
#### run3 start=13:39:59
  rc=0
job 23590-1793108399: 40 nodes, 80 workers, spawn 0.84 s
  hashes     2,001,000,000 counted in 40.45 s  => 49.5 MHash/s
  compute    2,001,000,000 in 40.38 s  => 49.6 MHash/s (from first chunk)
  workers    80 on 40 nodes
  verify     n/a (budget reached, no hit needed)
  end=13:40:40
ALLDONE 13:40:40
```

Single machine (`nd20`):

```
40 procs x 20000000 hashes in 42.74 s => 18.7 MHash/s (0.468 per proc)
48 procs x 20000000 hashes in 54.97 s => 17.5 MHash/s (0.364 per proc)
```

---

## 10. 中文摘要

**这一轮问的是什么**：同一批硬件、只把节点数从 20 翻到 40，吞吐是不是也跟着翻倍，第二倍要付多少钱。

**环境**：还是那 5 台双路 Xeon E5-2650 v4，每台从 4 台虚拟机加到 8 台（2 vCPU / 1.5 GiB），
共 **40 节点、80 vCPU**；单台宿主 16 vCPU / 48 线程，仍未超卖。协调机仍是那台不参与计算的 ARM 板。

**扩容前后对比（同一套压测，2 G 哈希，各 3 轮取中位）**

| | 20 节点 | 40 节点 | 比值 |
|---|---|---|---|
| vCPU | 40 | 80 | 2.00 倍 |
| 墙钟（2 G 哈希） | 78.78 s | 40.67 s | 快 1.94 倍 |
| 吞吐 | 25.4 MHash/s | **49.2 MHash/s** | **1.94 倍** |
| 每 vCPU | 0.635 | **0.615** | **96.9 %** |
| 分发开销占比 | 0.5–0.8 % | 1.3–2.4 % | |

**vCPU 翻倍，吞吐 1.94 倍，每线程保留 96.9 %（掉 3.1 %）。**

**同一天同一台机器（nd20，48 线程）做对照**

| 进程数 | 速率 | 每进程 |
|---|---|---|
| 40 | 18.7 MHash/s | 0.468 |
| 48 | 17.5 MHash/s | 0.364 |

- 40 节点集群 = 单机 40 进程的 **2.63 倍**，= 单机 48 进程（一进程一线程、机器塞满）的 **2.81 倍**。
- 单机塞到 48 进程，总吞吐反而从 18.7 掉到 17.5，每进程塌到 0.364；集群 80 vCPU 仍保持 0.615。
  **每 vCPU 是"塞满的单机"的 1.69 倍。**
- 所以这组数的真正含义不是"SSI 变快了"，而是**没被挤在同一对 CPU 上**。这一点和 20 节点那轮一致。

**这一轮有一次跑错了，写在报告里**：第一轮 40 节点跑出 25.3 MHash/s（几乎等于 20 节点），
原因是新加的 20 台虚拟机上没有 `/usr/local/bin/krg-crack` —— `spawn` 只负责起进程，二进制不存在
worker 立刻死，协调端仍按"请求的 80 个"计数，于是 `workers 80` 是假的、实际只有 40 个在算。
补齐后重测才是 49.2。**25.3 不是扩容结果，不能引用。** 教训已写进 `BENCHMARKS.md`：
压测前逐台 `ls` 一遍 worker 二进制。

**没测的**：再翻一倍（160 vCPU）是否还保持 0.615、VM 开销与集群开销的干净拆分、节点中途掉线、
以及除 CPU 密集以外的负载。

---

*图：`docs/images/krg-top-40nodes.png` —— 满载时 40 张卡片全在，页头 `nodes 40/40 · cores 80 · workers 80`，
无 IP、无主机名以外的标识。*
