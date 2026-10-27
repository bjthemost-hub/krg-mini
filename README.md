# krg-mini — a minimal single system image on Linux

> **AI-assisted project.** Built in pair-programming with an AI coding agent
> (OpenAI Codex) and a human maintainer; every change was compiled, installed and
> measured on real machines. See [AI disclosure](#ai-disclosure).

Kerrighed was a Single System Image (SSI) cluster: many Linux boxes that look like
one machine — one process table, one memory, one scheduler, live process migration.
It died with 2.6.30. **krg-mini** keeps the idea and throws away almost everything
else: 272 lines of kernel module plus three userspace tools (~1100 lines total),
built against a stock Ubuntu 22.04 kernel, no patch required.

An SSI is four things at once. Here is where this project stands — no more, no less:

| property | status |
|---|---|
| one process space — `ps` / `kill` on any node reaches every task | **yes**: `/dev/krgview` exports each node's table, merged in userspace, global pid = `node_id << 24 \| pid` |
| a task can be created on any node | **partial**: `SPAWN` starts a process on another node, but it is not part of the caller's process tree |
| one file system — same path, same bytes everywhere | **no**: `krgsh ls` runs `ls` per node and prints the results side by side |
| one memory — an address range visible from every node | **no** |

The next three steps, in order: a shared `/krg` (NFS first, so paths and bytes
agree), then a process tree that spans nodes (remote fork with fd/pipe objects
forwarded), then a paged DSM. Until the last one lands, this is honest to call a
*partial* SSI.

What actually works today:

| capability | how |
|---|---|
| one process table across nodes | `/dev/krgview` exports each node's task list; `krgtool show` merges them. Every task carries a **global pid** = `node_id << 24 \| pid`. |
| create / kill a task on another node | write `SPAWN <cmd>` or `KILL <gpid>` to `/dev/krgview`; the kernel uses `call_usermodehelper` / `kill_pid` |
| cluster shell with built-in commands | `krgsh` — `free`, `top`, `ls`, `bash` run on every node and come back merged |
| load-aware placement + retry | `krg-pi` asks every node for its run queue, places shards on the least loaded ones, and re-runs shards whose worker died |
| throughput workload | `krg-crack` — SHA-256 preimage search; workers pull 1M-nonce chunks from a queue, the winner is re-hashed locally |
| live status board | `krg-top` — one HTML page with every node's CPU, memory, load, kernel and worker count; reads through `krgtool exec` so unreachable nodes show as unreachable |

What is **not** here: process migration, distributed shared memory, a distributed
filesystem, a global scheduler in the kernel. Those are the hard 90% of Kerrighed.

## Repository layout

```
module/krg_taskview.c   the whole kernel side: /dev/krgview (read table, write SPAWN/KILL)
user/krgtool.c          local/ serve/ show/ spawn/ kill/ load/ mem/ exec
user/krgsh.py           cluster shell: free / top / ls / bash (+ nodes, show)
user/krg-pi.py          distributed pi to N digits, used as the SSI demo workload
user/krg-crack.py       SHA-256 preimage search, used to measure cluster throughput
user/krg-top.py         live status board (HTTP + one HTML page, stdlib only)
deploy/install.sh       build + install on one node
deploy/krg-mini.service systemd unit (modprobe + krgtool serve 7700)
deploy/krg-nodes.list.example
docs/DESIGN.md          data structures, wire protocol, coordinator logic
docs/BENCHMARKS.md      measured numbers on a 13-node setup + how to reproduce
```

## Requirements

- Linux with matching kernel headers (`linux-headers-$(uname -r)`), gcc
- Python 3.8+ on every node that runs `krgsh` / `krg-pi`
- A trusted network. **`krgtool serve` has no authentication** — it will run any
  shell command it is asked to run. Keep port 7700 on a private network.

## Quick start (two boxes)

```sh
# on every node: pick a unique node_id (1..255) and install
sudo ./deploy/install.sh 1          # head node
sudo ./deploy/install.sh 2          # worker

# on every node: same cluster list
sudo cp deploy/krg-nodes.list.example /etc/krg-nodes.list   # then edit
```

Verify:

```sh
krgsh -c 'bash hostname; uname -r'   # one block per node
krgsh -c top -n 10                   # merged process table
krgsh -c free                        # cluster memory
krgsh -c 'ls -l /var/log'            # same path on every node
```

`krgsh` also runs interactively, and reads commands from stdin, so it can be
scripted:

```sh
printf 'free\ntop -n 5\n' | krgsh
```

Anything that is not a built-in goes through `bash`, exactly like
`krgsh -c 'bash <command>'`.

## The demo workload

```sh
krg-pi --digits 100        # 13 nodes, self-checked against Machin's formula
krg-pi --digits 50000
```

Each shard is a Chudnovsky term range; the coordinator asks each node for its
runnable-task count first, sorts nodes by load, and retries any shard whose worker
does not answer within the deadline (3 rounds max). The result is verified against
an independent Machin computation, so a wrong answer cannot pass silently.

## Measured (2026-10-27, 13 nodes)

| workload | 13 nodes | one VM (1 shard) | one VM forced to run 13 shards |
|---|---|---|---|
| pi(100), 5 runs, median | **307 ms** | 263 ms | 653 ms |
| pi(50000), 1 run | 7915 ms | 7090 ms | 11701 ms |

pi(100) is 0.1–0.2 ms of actual arithmetic, so those numbers are coordination
overhead, not speed-up. At 50000 digits the cluster does not beat a single VM
either: 4.8 s of every run is the serial Machin self-check and 2.3 s is the
start-up term of the last shard. See `docs/BENCHMARKS.md` for the breakdown;
the sharding scheme is the obvious thing to fix.

## Measured throughput (2026-10-27, 15 nodes)

`pi` is a latency workload and loses to one VM, so the second workload is pure
throughput: `krg-crack` searches a SHA-256 hash prefix of 12 hex zeros, in
1,000,000-nonce chunks pulled from the coordinator, with `--budget` pinning the
amount of work so a lucky hit cannot flatter the number.

| rig | threads | work | wall | rate |
|---|---|---|---|---|
| one VM (2 vCPU) | 2 | 60 M | 45.6 s | 1.3 MHash/s |
| 12 VMs over 3 hosts | 24 | 601 M | 40.3 s | 14.9 MHash/s |
| one 48-thread server | 48 | 1.00 G | 69.1 s | 14.5 MHash/s |
| **15-node cluster (3 servers + 12 VMs)** | **168** | **2.00 G** | **46.3 s** | **43.3 MHash/s** |

- The cluster scales: 43.3 MHash/s is **2.4x** what one of those servers does with
  no cluster layer at all (18.3 MHash/s, same loop in bare `fork()`+`hashlib`), and
  **33x** a single 2-vCPU VM.
- The win is core count, not the SSI. Per thread the cluster gets 0.26 MHash/s
  against 0.38 on bare metal — about 68% — because the coordinator is one Python
  process doing a socket round trip per chunk, and the head node also hosts four VMs.
- 24 VM threads land on the same 14.9–14.5 MHash/s as one 48-thread box: those 48
  are 24 physical cores with SMT, and CPython's SHA-256 gains almost nothing from
  the sibling thread.
- Fanning out 168 workers across 15 nodes costs 4.9 s of the 46 s run.
- **Where the coordinator runs matters more than its clock speed.** The same 2 G
  of work takes 43.7 MHash/s (median of 3) when the coordinator is one of the
  worker nodes and also hosts four VMs, and 49.2 MHash/s (+12.6%) when it is a
  separate ARM box that hashes nothing — fan-out drops from ~4.9 s to ~2 s. What
  the coordinator needs is not being preempted, not a fast core.

Full environment, per-node numbers and the exact commands are in
`docs/BENCHMARKS.md`.
## Status

A teaching prototype. It is honest about what it does, small enough to read in an
afternoon, and a reasonable starting point if you want to grow one SSI feature at
a time on a modern kernel.

## 中文简介

**krg-mini 是把 Kerrighed 的"单系统映像（SSI）"思路做到最小的一份实现。**
Kerrighed 是 Linux 2.6 时代的 SSI 集群 —— 一堆机器看起来像一台：一张进程表、一份内存、
一个调度器，进程还能在线迁移。它随 2.6.30 一起停在了过去。krg-mini 只留下骨架：
**一个 272 行的内核模块 + 三个用户态工具，不用打内核补丁**，在 Ubuntu 22.04 的原版内核上直接编译加载。

| 已经能做到 | 怎么做的 |
|---|---|
| 一张跨节点的进程表 | 内核模块导出 `/dev/krgview`，每台机器一张表，用户态合并；每个进程有**全局 pid** = `节点号 << 24 \| pid` |
| 在别的节点上建 / 杀进程 | 往 `/dev/krgview` 写 `SPAWN <命令>` 或 `KILL <全局pid>`，由**内核**用 `call_usermodehelper` / `kill_pid` 执行 |
| 集群 shell | `krgsh`：`free` / `top` / `ls` / `bash` 一条命令扇出到所有节点，结果按节点分块显示 |
| 按负载分配 + 失败重试 | `krg-pi`：先问每台机器的运行队列长度，把分片放到最闲的节点，超时没回来的分片换节点重算（最多 3 轮） |
| 吞吐型负载 | `krg-crack`：SHA-256 原像搜索。worker 从队列里按 100 万个 nonce 一块地领，命中的那块在本地重算一遍再报，防止误报 |
| 实时状态面板 | `krg-top`：一个 HTML 页面显示每个节点的 CPU / 内存 / 负载 / 内核 / worker 数；数据也是走 `krgtool exec` 取的，连不上的节点直接标成连不上 |

SSI 是四件事凑齐才叫"一台机器"。这里照实标出到哪一步：

| SSI 的构成 | 现在的状态 |
|---|---|
| 单一进程空间（任意节点 `ps` / `kill` 都能碰到所有进程） | **有**：`/dev/krgview` 导出各节点进程表，用户态合并；全局 pid = `节点号 << 24 \| pid` |
| 任务可以在任意节点上创建 | **部分**：`SPAWN` 能在别的节点起进程，但它不是调用者那棵进程树的一部分 |
| 单一文件系统（同一路径、同一份数据） | **没有**：`krgsh ls` 是每台各列一遍再拼起来 |
| 单一内存（一段地址空间多节点可见） | **没有** |

接下来三步：先 `/krg` 共享（先上 NFS，让路径和数据一致），再做跨节点的进程树（远程 fork，
继承的 fd / 管道 / socket 这些对象要自己转发，不能在远端按路径重新 open），最后才是按页拉取的 DSM。
第三块落地之前，它只能叫**部分 SSI**。
**没做的**（也就是 Kerrighed 真正难的那 90%）：进程迁移、分布式共享内存、共享文件系统、
内核里的全局调度。这里的 `ls` 只是"同一个路径在每台机器各列一遍"，因为节点之间并没有共享文件系统。

实测（2026-10-27，13 个节点 = 1 台物理机 + 12 台虚拟机，Ubuntu 22.04 / 内核 5.15）：
`krgsh` 一条命令汇总 13 台机器的进程表、内存、目录；`krg-pi --digits 100` 13/13 节点应答、
结果用独立的 Machin 公式逐位校验通过，中位 307 ms；`--digits 50000` 用时 7.9 s。
**100 位这个量级本来只有 0.1–0.2 ms 的算术量，307 ms 全是协调开销** ——
数字怎么看、怎么复现，都写在 `docs/BENCHMARKS.md` 里，没有粉饰。

**吞吐型负载**（`krg-crack`：SHA-256 原像搜索，用 `--zeros 12 --budget` 定死工作量）：
单台 2 vCPU 虚拟机 1.3、12 台虚拟机（24 vCPU）14.9、单台 48 线程物理机 14.5，
**15 节点集群（3 台物理机 + 12 台虚拟机，168 个 worker）43.3 MHash/s** ——
是单台物理机**裸机**（18.3）的 2.4 倍、单虚拟机的 33 倍。
但每线程只有 0.26（裸机 0.38，约 68%）：**扩出来的收益来自核数，不是来自 SSI**，
协调端是单个 Python 进程，168 个 worker 的分发本身要 4.9 s。

**协调端放在哪儿，比它跑多快更重要**：同一份 2 G 工作量，协调端和 worker 挤在同一台机器上
（而且那台机器还扛着 4 台虚拟机）是中位 43.7 MHash/s；挪到一台**不参与计算**的 ARM 板子上是
**49.2 MHash/s（+12.6%）**，分发开销从约 4.9 s 降到约 2 s。协调端一个哈希都不算，
它要的是"不被打断"，不是单核快。

**实时状态面板**（`krg-top`）：一个 HTML 页面显示每台机器的 CPU、内存、负载、内核和 worker 数，
走 `krgtool exec` 取数，连不上的节点就照实显示连不上。

**安全提醒**：`krgtool serve` 没有任何认证，能执行任意 shell 命令，只能跑在可信内网。

用法、协议、数据结构见 `README.md` 上半部分和 `docs/DESIGN.md`。

## AI disclosure

本项目是**人和 AI 结对做出来的**，这里把话说清楚：

- **AI 做的**：起草内核模块（`krg_taskview.c`）、`krgtool`、`krgsh`、`krg-pi` 的代码与文档，
  按人的要求做实验、跑测量、写报告。
- **人做的**：定方向与边界（从 Kerrighed 里取哪一小块、不做什么）、审阅每一笔改动、
  在真机上编译安装并验证、决定什么算"完成"。
- **"验证过"是什么意思**：每个里程碑都在目标内核上编译、装到 13 台节点上、用真实输出核对 ——
  例如合并进程表、跨节点建/杀进程、13 节点算 π 并用独立的 Machin 公式逐位比对。
  没有"应该能跑"这种结论。
- **边界**：这是一个原型，不是生产系统；AI 写的代码同样会有错，欢迎按普通开源项目的标准审视它。

*English:* this project was written in pair-programming with an AI coding agent
(OpenAI Codex). The AI drafted the kernel module, the userspace tools and the
docs; the human set the scope, reviewed every change, and built, installed and
measured everything on real hardware. "Verified" here always means an actual build
plus actual output on the 13-node cluster (merged process table, cross-node
spawn/kill, pi to N digits checked bit-for-bit against Machin's formula) — never
"should work". It is a prototype; review it like any other open source code.
## License

GPL-2.0. The kernel module declares `MODULE_LICENSE("GPL")`; everything is under
the same terms (see `LICENSE`).