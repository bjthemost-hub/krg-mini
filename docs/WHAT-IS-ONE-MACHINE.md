# What "one machine" means here — measured, not claimed

**Status as of 2026-10-27.** This document is the project's own statement about how
far the illusion goes. It exists because "cluster that looks like one machine" is
easy to say and almost always overstated. Everything below is checkable in the tree.

A Single System Image is four properties. They are listed here in the order they are
usually *claimed* — and in the order they are actually hard.

| # | property | what a user would see | what this tree does today | verdict |
|---|---|---|---|---|
| 1 | **one process space** | `ps` on any node lists every task in the cluster; `kill <pid>` works from anywhere | each node exports its task table through `/dev/krgview`; userspace merges them; every task carries a global pid = `node_id << 24 \| pid` | **implemented** |
| 2 | **one CPU** | one run queue, one scheduler; a new task lands on the least loaded core of the whole cluster without anyone choosing | reads `nproc` and load per node and *prints a sum*; `krg-pi` picks nodes in **userspace** before spawning | **not implemented** |
| 3 | **one memory** | an address range allocated on node A can be read and written from node B, coherently | sums `MemTotal` / `MemFree` per node and prints one "cluster" line | **not implemented** |
| 4 | **one file system** | the same path resolves to the same bytes everywhere | `krgsh ls` runs `ls` on every node and prints the results side by side | **not implemented** |

## 1. One process space — implemented

`/dev/krgview` exports each node's task list; `krgtool show` merges the lists. A task
born on node 7 shows up on node 2 with pid `(7 << 24) | pid`, and `krgtool kill`
routes the request back to node 7's kernel. This is the one property that behaves
like a single machine today.

It is a **birth identity**, not a location: the pid says where the task was created,
not where it runs now (nothing migrates yet).

## 2. One CPU — not implemented

What exists:

- `krgsh top` / `krg-top` read `nproc`, `/proc/stat` and `/proc/loadavg` from every
  node and **add them up**. `krgsh.py:148` opens with `sum(...)`; that is a
  presentation of 20 independent numbers, not a scheduler.
- `krg-pi` asks every node for its run queue and places shards on the least loaded
  one — **in userspace, before the work starts**, and only for work it creates itself.

What does not exist: a single run queue. Each node's kernel schedules its own tasks
with its own CFS instance. A task cannot be moved to another node's CPU. There is no
place in the system where a scheduling decision covering two nodes is made.

So the honest sentence is: **the cluster reports one CPU; it does not have one.**

## 3. One memory — not implemented

`krgsh free` prints a `cluster` line whose total is the arithmetic sum of every node's
`MemTotal`, and used is the sum of the per-node used values (`krgsh.py:128-135`).

That is accounting, not memory. A process on node A cannot name, map, read or write a
byte that lives on node B. There is no shared address space, no page ownership, no
fault forwarding, no coherence protocol. Nothing in the tree would be able to serve a
page fault for a page that is resident elsewhere.

So the honest sentence is: **the cluster reports one big memory; it does not have one.**

## 4. One file system — not implemented

`krgsh ls <path>` runs `ls` on every node, because nodes do not share a file system.
Same path, different bytes: a file written on node 3 is invisible on node 9.

## Why write this down

Because the interesting part of an SSI project is the gap between the four claims,
and a project that blurs the gap cannot be reviewed, tested or trusted. Anyone can
compile this tree and check each row above against the code and against a running
cluster.

## What "v0.1" means for this project

The next milestone is named **khz v0.1** and is deliberately defined by properties,
not by dates:

**khz v0.1 = properties 1, 2 and 3 above, each with an acceptance test anyone can rerun.**

| property | acceptance test for v0.1 |
|---|---|
| one process space | `ps`/`kill` from any node reaches every task; already passing |
| one CPU | a task created on any node is placed by one policy covering all nodes, and the placement decision is visible from every node — no per-node choice by the caller |
| one memory | allocate on node A, read and write the same bytes from node B, with the write visible to A; the test does not go through a file or a socket |
| (file system) | **not in v0.1** — stays a known gap, documented here |

Property 4 (one file system) is explicitly **out of scope for v0.1**. The `/krg` tree
comes after, on top of whatever the first three turn out to be.

---

## 中文

**截至 2026-10-27 的实况。** 这份文档只回答一个问题：现在这套东西，离"看上去是一台机器"还有多远。
SSI 是四条性质，下面按"通常被声称的顺序"、也是"实际难度的顺序"逐条对照，每条都能在代码里核对。

| # | 性质 | 用户会看到什么 | 现在实际是什么 | 结论 |
|---|---|---|---|---|
| 1 | 单一进程空间 | 任意节点 `ps` 能看到全集群的进程，`kill` 跨节点有效 | 每节点通过 `/dev/krgview` 导出任务表，用户态合并，全局 pid = `节点号 << 24 \| pid` | **已实现** |
| 2 | 一块 CPU | 一个运行队列、一个调度器；新任务自动落到全集群最闲的核上，不需要调用方挑 | 只是把各节点 `nproc` / 负载**加起来显示**；`krg-pi` 是**用户态**先挑节点再起进程 | **未实现** |
| 3 | 一大片内存 | 在 A 节点分配的一段地址，B 节点能读能写，且 A 能看到 | 把各节点 `MemTotal` / `MemFree` **相加**打印一行 "cluster" | **未实现** |
| 4 | 单一文件系统 | 同一个路径在每台机器上是同一份字节 | `krgsh ls` 在每台机器各跑一遍 `ls`，结果并排打印 | **未实现** |

**一句话**：现在是"**一个进程表 + 一堆各自算的机器**"。CPU 和内存只是汇总显示，不是共享 ——
集群"报告"了一块 CPU 和一大片内存，但它并不"拥有"这两样东西。

`/dev/krgview` 里的 pid 只说明**出生在哪个节点**，不说明现在跑在哪（目前也没有迁移）。

### 为什么要把这个写下来

SSI 项目真正有意思的地方，就是这四条声称之间的差距。把这层差距抹平，项目就没法被审阅、被测试、被信任。
这张表任何人拿这份代码 + 一个跑起来的集群都能逐条复验。

### v0.1 的定义

下一个里程碑叫 **khz v0.1**，用性质定义，不用日期定义：

**khz v0.1 = 上面的第 1、2、3 条，每条都带一个别人能重跑的验收测试。**

| 性质 | v0.1 的验收标准 |
|---|---|
| 单一进程空间 | 任意节点 `ps` / `kill` 覆盖全部任务（已达到） |
| 一块 CPU | 在任意节点创建的任务，由**一个覆盖全集群的策略**决定落点，且这个决定在每个节点都看得到；不由调用方逐节点挑 |
| 一大片内存 | 在 A 节点分配，在 B 节点读写同一段字节，写回对 A 可见；测试**不经过文件、不经过 socket** |
| （单一文件系统） | **不进 v0.1**，作为已知缺口留在本文档里 |

第 4 条（单一文件系统）明确不在 v0.1 范围内，`/krg` 放在这之后做。