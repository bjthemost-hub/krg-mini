# krg-mini — a minimal single system image on Linux

> **AI-assisted project.** Built in pair-programming with an AI coding agent
> (OpenAI Codex) and a human maintainer; every change was compiled, installed and
> measured on real machines. See [AI disclosure](#ai-disclosure).

Kerrighed was a Single System Image (SSI) cluster: many Linux boxes that look like
one machine — one process table, one memory, one scheduler, live process migration.
It died with 2.6.30. **krg-mini** keeps the idea and throws away almost everything
else: 272 lines of kernel module plus three userspace tools (~1100 lines total),
built against a stock Ubuntu 22.04 kernel, no patch required.

What actually works today:

| capability | how |
|---|---|
| one process table across nodes | `/dev/krgview` exports each node's task list; `krgtool show` merges them. Every task carries a **global pid** = `node_id << 24 \| pid`. |
| create / kill a task on another node | write `SPAWN <cmd>` or `KILL <gpid>` to `/dev/krgview`; the kernel uses `call_usermodehelper` / `kill_pid` |
| cluster shell with built-in commands | `krgsh` — `free`, `top`, `ls`, `bash` run on every node and come back merged |
| load-aware placement + retry | `krg-pi` asks every node for its run queue, places shards on the least loaded ones, and re-runs shards whose worker died |

What is **not** here: process migration, distributed shared memory, a distributed
filesystem, a global scheduler in the kernel. Those are the hard 90% of Kerrighed.

## Repository layout

```
module/krg_taskview.c   the whole kernel side: /dev/krgview (read table, write SPAWN/KILL)
user/krgtool.c          local/ serve/ show/ spawn/ kill/ load/ mem/ exec
user/krgsh.py           cluster shell: free / top / ls / bash (+ nodes, show)
user/krg-pi.py          distributed pi to N digits, used as the SSI demo workload
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

**没做的**（也就是 Kerrighed 真正难的那 90%）：进程迁移、分布式共享内存、共享文件系统、
内核里的全局调度。这里的 `ls` 只是"同一个路径在每台机器各列一遍"，因为节点之间并没有共享文件系统。

实测（2026-10-27，13 个节点 = 1 台物理机 + 12 台虚拟机，Ubuntu 22.04 / 内核 5.15）：
`krgsh` 一条命令汇总 13 台机器的进程表、内存、目录；`krg-pi --digits 100` 13/13 节点应答、
结果用独立的 Machin 公式逐位校验通过，中位 307 ms；`--digits 50000` 用时 7.9 s。
**100 位这个量级本来只有 0.1–0.2 ms 的算术量，307 ms 全是协调开销** ——
数字怎么看、怎么复现，都写在 `docs/BENCHMARKS.md` 里，没有粉饰。

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