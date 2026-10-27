# Report — 20 nodes, 5 hosts × 4 VMs: throughput, overhead, and what it means

**Date:** 2026-10-27 · **Rig:** 20 VirtualBox guests on 5 physical hosts · **Status:** measured

This is the write-up of one benchmark round: what was run, what came back, and what
the numbers do and do not support. Raw log is quoted at the end; nothing here is
estimated unless it says so.

---

## 1. Question

The project claims to be a *partial* SSI: one process table across nodes, tasks
created on other nodes, a merged cluster shell and a live status board. None of that
says anything about whether the thing is **useful** at scale.

So: on a fleet of VMs, does the cluster actually deliver more work than one machine,
and how much does the cluster layer itself cost?

The honest way to ask that is with a workload where the cluster layer is the only
variable — same code, same inner loop, same amount of work, with and without nodes
in between.

## 2. Rig

| item | value |
|---|---|
| hosts | 5 × 2-socket Xeon E5-2650 v4 (24 cores / 48 threads each), 32–64 GiB |
| nodes | 20 VirtualBox guests, 4 per host, 2 vCPU and 2 GiB each |
| node OS | Ubuntu 22.04.5, kernel 5.15.0-198, same image everywhere |
| total vCPU | 40 |
| host load | 8 vCPU per 48-thread host — the hosts are **not** oversubscribed |
| cluster software | `krg_taskview.ko` + `krgtool serve 7700` on every node, same 20-line node list |
| coordinator | a separate 12-core ARM board on the same switch, **not** a node, hashes nothing |
| interconnect | one flat LAN, VMs NATed through their host |

Two things worth stating because they were different in earlier rounds:

- **Physical hosts are hosts only.** Their own `krg-mini` is stopped and disabled, so
  the compute budget is exactly 40 vCPU and no host is fighting its guests.
- **The coordinator is off the workers.** It runs on a slow ARM core, three
  generations behind a Xeon — and that is fine, because it computes nothing.

## 3. Method

**Workload.** `krg-crack`: find a nonce so `sha256("<prefix><nonce>")` starts with 12
hex zeros. The search space is cut into 1,000,000-nonce chunks; 40 workers (2 per
node, one per vCPU) pull chunks from a queue on the coordinator and report back.

**Fixed budget, not "find a hit".** `--budget 2000000000` makes the run stop after a
fixed number of hashes and report the rate. A lucky early hit cannot flatter the
number, and two runs are directly comparable. `--zeros 12` guarantees no hit happens
during the 2 G of work.

**Timing.** Wall clock inside the process: before node discovery, after the last
chunk. It includes `exec nproc` probes, 40 `spawn` calls, the chunk queue, and a
batch of small socket round trips. That is the point — the cluster layer is included
by design.

**Baseline.** `raw_sha.py`, the same `hashlib.sha256(...).hexdigest()` loop with the
same 100,000-hash inner chunk, run as plain `fork()`ed processes on one host, no
cluster layer at all. Same machine class as the hosts, same kernel, same Python.

Three cluster runs of 2 G each, plus one 50 M warm-up that is reported but not used
for any conclusion (it is the ramp-up, not steady state).

## 4. Results

### 4.1 The cluster

| run | hashes counted | wall | rate | fan-out |
|---|---|---|---|---|
| warm-up | 50,000,000 | 3.21 s | 15.6 MHash/s | 0.31 s |
| 1 | 2,000,000,000 | 78.23 s | 25.6 MHash/s | 0.39 s |
| 2 | 2,000,000,000 | 78.78 s | 25.4 MHash/s | 0.64 s |
| 3 | 2,001,000,000 | 78.71 s | 25.4 MHash/s | 0.44 s |

**Median 25.4 MHash/s over 40 vCPU = 0.635 MHash/s per vCPU.**

Run 3 counted 1 M more than budget: the coordinator hands out whole chunks and stops
when the counter passes the budget, so the last chunk overshoots by at most one.

All 20 nodes answered on every run — no `unreachable`, no `no progress for 30 s`, no
worker died. `workers 40 on 20 nodes` in all four runs.

### 4.2 The same loop with no cluster layer

One host, 48 threads, 24 physical cores, SMT on:

| processes | hashes each | wall | rate | per process |
|---|---|---|---|---|
| 8 | 50 M | 61.25 s | 6.5 MHash/s | 0.816 |
| 20 | 40 M | 49.22 s | 16.3 MHash/s | 0.813 |
| 40 | 20 M | 42.20 s | 19.0 MHash/s | 0.474 |

The 20 → 40 step is the interesting one: past one process per physical core, SMT
halves per-process throughput (0.813 → 0.474) and total only rises 16%.

## 5. What it means

**1. Cluster throughput in the units people care about**

- **1.56× a single 20-core machine** — 25.4 vs 16.3 MHash/s — at equal *useful* core
  count and equal per-core efficiency assumptions on the bare side.
- **1.34× a single machine running 40 processes** (19.0). That is the more provocative
  comparison: 40 workers on one box lose 40% per process to SMT, while 40 workers
  spread over 5 boxes keep 0.635.
- At 40 vCPU the cluster is at **78%** of bare-metal per-process rate (0.635 / 0.813).

**2. Where the missing 22% goes**

- The coordinator is one Python process: per 1 M-hash chunk, a worker does one
  connect / `GET` / read / close against it. At 25 MHash/s that is ~25 round trips
  per second in total, which is small but not zero.
- Fan-out is negligible: 0.39–0.64 s out of 78 s (< 1%). Earlier rounds paid 4.9 s of
  46 s because the coordinator was itself a node on an oversubscribed host.
- The rest is VM overhead and clock behaviour, not measured separately here — see
  §6.

**3. This is a scaling result, not an SSI result**

Nothing in this benchmark uses a *single system image*. `krg-crack` could run on
`ssh` + a chunk server. What it measures is: can this cluster's control plane move
work around without eating the win? Answer: yes, at ~1% fan-out and ~78% per-core
efficiency, on VMs.

The SSI claims — one process table, remote `SPAWN`, a merged shell — cost nothing at
idle and are not exercised here. They need their own test (and the shared filesystem
and DSM are still missing, see the README status table).

**4. Against earlier rounds**

| rig | threads | rate | per thread |
|---|---|---|---|
| 3 servers + 12 VMs (nodes include the servers) | 168 | 49.2 | 0.29 |
| 20 VMs, hosts not nodes (this report) | 40 | 25.4 | 0.635 |

The 15-node rig was 168 workers on 144 physical threads, with the coordinator on one
of them. Its per-thread number mostly measures oversubscription. **The two totals are
not comparable** — 49.2 is larger only because it used 4× the threads.

![20-node cluster at full load](images/krg-top-20nodes.png)

`krg-top` during run 2: `20/20` nodes, `cores 40`, cluster CPU `99.8 %`, `workers 40`,
every card at ~100% CPU and 0.3 of 1.9 GiB used (this workload is CPU-only).

## 6. Limits — what this does not say

- **One workload.** SHA-256 in CPython, CPU-bound, embarrassingly parallel, 1 M-chunk
  granularity. Nothing here predicts a workload with shared state, IPC, or fine
  granularity (< 10 k work units per chunk).
- **One topology.** 40 vCPU on 5 hosts. Scaling to 100+ nodes was not measured; the
  coordinator is a single Python process and would be the first thing to break.
- **VM overhead not isolated.** The gap to 78% bundles the cluster layer with
  VirtualBox's own cost. A clean split needs a bare-metal node in the same run —
  **not done**.
- **No latency numbers here.** `krg-pi` (a latency workload) was measured on the
  earlier 13-node rig only.
- **No failure testing.** Every run had all 20 nodes healthy. What happens when a node
  dies mid-run is implemented (workers retry, shards re-run) but not measured in this
  round.
- **Timing resolution.** Wall clocks come from `time.time()` on the coordinator, ms
  resolution; runs are 78 s, so this is not a limiting factor.

## 7. Reproduce

```sh
# on every node (2 vCPU, Ubuntu 22.04, matching headers)
sudo ./deploy/install.sh <node_id>          # builds + installs module and tools
# node list: one "name host:7700" line per node, identical on all nodes

# on the coordinator (any box, does not need to be a node)
krg-crack --zeros 12 --budget 2000000000 \
          --nodes /etc/krg-nodes.list --workers-per-node 2

# baseline: same loop, no cluster
python3 raw_sha.py 20 40000000              # 20 procs x 40 M = 800 M hashes
```

The screenshot is the live board: `krg-top`, one HTML page, polling every node through
`krgtool exec`.

## 8. Raw log (coordinator, unabridged)

```
warm start=13:05:33
  warm rc=0
job 22019-1793106334: 20 nodes, 40 workers, spawn 0.31 s
  krg11-1   2 workers
  krg11-2   2 workers
  krg11-3   2 workers
  krg11-4   2 workers
  krg12-1   2 workers
  krg12-2   2 workers
  krg12-3   2 workers
  krg12-4   2 workers
  krg13-1   2 workers
  krg13-2   2 workers
  krg13-3   2 workers
  krg13-4   2 workers
  krg20-1   2 workers
  krg20-2   2 workers
  krg20-3   2 workers
  krg20-4   2 workers
  krg21-1   2 workers
  krg21-2   2 workers
  krg21-3   2 workers
  krg21-4   2 workers
  hashes     50,000,000 counted in 3.21 s  => 15.6 MHash/s
  compute    50,000,000 in 3.11 s  => 16.1 MHash/s (from first chunk)
  workers    40 on 20 nodes
#### run1 start=13:05:37
  rc=0
job 22087-1793106337: 20 nodes, 40 workers, spawn 0.39 s
  krg11-1   2 workers
  krg11-2   2 workers
  krg11-3   2 workers
  krg11-4   2 workers
  krg12-1   2 workers
  krg12-2   2 workers
  krg12-3   2 workers
  krg12-4   2 workers
  krg13-1   2 workers
  krg13-2   2 workers
  krg13-3   2 workers
  krg13-4   2 workers
  krg20-1   2 workers
  krg20-2   2 workers
  krg20-3   2 workers
  krg20-4   2 workers
  krg21-1   2 workers
  krg21-2   2 workers
  krg21-3   2 workers
  krg21-4   2 workers
  hashes     2,000,000,000 counted in 78.23 s  => 25.6 MHash/s
  compute    2,000,000,000 in 78.11 s  => 25.6 MHash/s (from first chunk)
  workers    40 on 20 nodes
  verify     n/a (budget reached, no hit needed)
  end=13:06:55
#### run2 start=13:06:55
  rc=0
job 22187-1793106416: 20 nodes, 40 workers, spawn 0.64 s
  krg11-1   2 workers
  krg11-2   2 workers
  krg11-3   2 workers
  krg11-4   2 workers
  krg12-1   2 workers
  krg12-2   2 workers
  krg12-3   2 workers
  krg12-4   2 workers
  krg13-1   2 workers
  krg13-2   2 workers
  krg13-3   2 workers
  krg13-4   2 workers
  krg20-1   2 workers
  krg20-2   2 workers
  krg20-3   2 workers
  krg20-4   2 workers
  krg21-1   2 workers
  krg21-2   2 workers
  krg21-3   2 workers
  krg21-4   2 workers
  hashes     2,000,000,000 counted in 78.78 s  => 25.4 MHash/s
  compute    2,000,000,000 in 78.70 s  => 25.4 MHash/s (from first chunk)
  workers    40 on 20 nodes
  verify     n/a (budget reached, no hit needed)
  end=13:08:14
#### run3 start=13:08:14
  rc=0
job 22287-1793106495: 20 nodes, 40 workers, spawn 0.44 s
  krg11-1   2 workers
  krg11-2   2 workers
  krg11-3   2 workers
  krg11-4   2 workers
  krg12-1   2 workers
  krg12-2   2 workers
  krg12-3   2 workers
  krg12-4   2 workers
  krg13-1   2 workers
  krg13-2   2 workers
  krg13-3   2 workers
  krg13-4   2 workers
  krg20-1   2 workers
  krg20-2   2 workers
  krg20-3   2 workers
  krg20-4   2 workers
  krg21-1   2 workers
  krg21-2   2 workers
  krg21-3   2 workers
  krg21-4   2 workers
  hashes     2,001,000,000 counted in 78.71 s  => 25.4 MHash/s
  compute    2,001,000,000 in 78.65 s  => 25.4 MHash/s (from first chunk)
  workers    40 on 20 nodes
  verify     n/a (budget reached, no hit needed)
  end=13:09:33
ALLDONE 13:09:33
```

---

## 9. 中文摘要

**这一轮要回答的问题**：把节点全部做成虚拟机之后，集群到底比一台机器多做多少活，集群层自己吃掉多少。

**环境**：5 台双路 Xeon E5-2650 v4 宿主，各跑 4 台虚拟机（2 vCPU / 2 GiB），共 **20 个节点、40 vCPU**；
宿主只当宿主（自己的 `krg-mini` 已停用），所以没有超卖；协调端放在一台**不参与计算**的 12 核 ARM 板子上。

**测法**：`krg-crack`（SHA-256 原像搜索）用 `--budget` 把工作量钉死成 2 G 哈希，跑 3 轮；
对照组是同一套 `hashlib` 循环、去掉集群层、在单台物理机上跑 8 / 20 / 40 个进程。

| 组别 | 线程 | 速率 | 每线程 |
|---|---|---|---|
| 集群（20 节点） | 40 | **25.4 MHash/s**（中位，3 轮） | 0.635 |
| 单机 20 进程 | 20 | 16.3 MHash/s | 0.813 |
| 单机 40 进程 | 40 | 19.0 MHash/s | 0.474 |

**结论（只说数据支持的）**：

- 集群是**单机 20 核的 1.56 倍**、**单机 40 进程的 1.34 倍**；每线程效率是裸机的 **78%**。
- 单机跑到 40 进程时被 SMT 拖到每进程 0.474，而集群摊到 5 台宿主上仍保持 0.635 ——
  **多机的好处在这里是"不被同一颗 CPU 挤"，不是"SSI 变快了"**。
- 40 个 worker 分发只要 0.39–0.64 s，占 78 s 的不到 1%。
- **这组数不能和 15 节点那组 49.2 直接比**：那组是 168 线程、宿主被压过头（每线程 0.29）。
- 这个负载**用不到 SSI**：换成 `ssh` + 一个发块的服务也能跑。它测的是"控制面会不会把收益吃掉"，
  答案是不会（约 1% 分发、78% 每核效率）。SSI 那几件事（单一进程表、远程建进程）在这个负载里没被考到。

**没测的**：延迟型负载（`krg-pi` 仍是 13 节点的旧数）、100 节点以上的扩展、节点中途掉线的行为、
VM 开销与集群开销的干净拆分（需要同轮加一台裸机节点）。
