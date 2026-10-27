# What "one machine" means here — measured, not claimed

**Status as of 2026-10-27.** This document is the project's own statement about how
far the illusion goes. It exists because "cluster that looks like one machine" is
easy to say and almost always overstated. Everything below is checkable in the tree.

A Single System Image is four properties. They are listed here in the order they are
usually *claimed* — and in the order they are actually hard.

| # | property | what a user would see | what this tree does today | verdict |
|---|---|---|---|---|
| 1 | **one process space** | `ps` on any node lists every task in the cluster; `kill <pid>` works from anywhere | each node exports its task table through `/dev/krgview`; userspace merges them; every task carries a global pid = `node_id << 24 \| pid` | **implemented** |
| 2 | **one CPU** | one run queue, one scheduler; a new task lands on the least loaded core of the whole cluster without anyone choosing | a sum for display; `krg-pi` picks in userspace; **since v0.1 the kernel itself places `SPAWN`ed tasks and reserves the target** — but ordinary `fork`/`exec` still lands locally and nothing rebalances | **partial** |
| 3 | **one memory** | an address range allocated on node A can be read and written from node B, coherently | named segments in `/dev/krgdsm`; `mmap` them and **plain pointer loads** work -- a fault for a page that is not here is handed to a helper thread, which pulls it from the home node. Pages cost memory only when touched; a reader that is not mapping can cap itself with a bounded cache and read a segment far bigger than its own RAM. Still **read-only** for the far side, no write coherence | **partial** |
| 4 | **one file system** | the same path resolves to the same bytes everywhere | `krgsh ls` runs `ls` on every node and prints the results side by side | **not implemented** |

## 1. One process space — implemented

`/dev/krgview` exports each node's task list; `krgtool show` merges the lists. A task
born on node 7 shows up on node 2 with pid `(7 << 24) | pid`, and `krgtool kill`
routes the request back to node 7's kernel. This is the one property that behaves
like a single machine today.

It is a **birth identity**, not a location: the pid says where the task was created,
not where it runs now (nothing migrates yet).

## 2. One CPU — partial: kernel initial placement

What exists:

- `krgsh top` / `krg-top` read `nproc`, `/proc/stat` and `/proc/loadavg` from every
  node and **add them up**. `krgsh.py:148` opens with `sum(...)`; that is a
  presentation of 20 independent numbers, not a scheduler.
- `krg-pi` asks every node for its run queue and places shards on the least loaded
  one — **in userspace, before the work starts**, and only for work it creates itself.
- **Since khz v0.1 the placement decision moved into the kernel** for tasks submitted
  through `SPAWN`. The module keeps a small load table (`LOAD <node> <runq> <seq>`,
  stale samples rejected), picks the node with the lowest `runq + reservations`,
  breaks ties round-robin, and **reserves** it under one lock so that a burst of
  callers cannot all pick the same idle node (`KRGV_IOC_PLACE` / `_RELEASE`).
  `krgtool spawn --auto` only collects the numbers and forwards the task to the node
  the kernel chose; it never re-picks. Acceptance test: with node 3 loaded
  (`runq 5`) and node 2 idle (`runq 2`), `spawn --auto` lands on node 2, and the task
  shows up there with `gpid = 2 << 24 | pid` in the merged `krgtool show`.

What does not exist: a single run queue. Each node's kernel schedules its own tasks
with its own CFS instance. A task cannot be moved to another node's CPU. There is no
place where a decision covering two nodes is made for an ordinary `fork`/`exec` —
only `SPAWN` goes through the kernel's placement, and nothing rebalances a task that
is already running.

So the honest sentence is: **the cluster reports one CPU; the kernel now places new
`SPAWN`ed tasks across nodes, but there is still no shared run queue.**

## 3. One memory — partial: a paged segment pulled on demand

`krgsh free` still prints a `cluster` line whose total is the arithmetic sum of every
node's `MemTotal` (`krgsh.py:128-135`). That part is accounting only, and on its own it
would be the same lie as before.

What changed is `/dev/krgdsm` (`module/krg_dsm.c`) plus `krgdsm` (`user/krgdsm.py`)
and `dsmread` (`user/dsmread.c`):

- A segment is a **name plus page slots**, not a block of pages. `ALLOC` reserves the
  slots; a page costs memory only once someone writes it. A 1 GiB segment that is
  never written costs a page table and nothing else.
- The **home** node writes pages and seals the segment. Any other node `ALLOC`s the
  same name and pulls the pages it wants through the cluster's existing channel
  (`DSMGET <name> <first> <count>`, up to 64 pages per request).
- A reader can give itself a **bounded cache** (`krgdsm alloc NAME PAGES --cache N`).
  Pulling page N+1 evicts the oldest one, so the node never holds more than N pages,
  and it can read a segment many times its own free memory.
- **It is also memory now, not just bytes.** `ATTACH` binds a file descriptor to a
  segment and `mmap` maps it. A fault for a page that is not resident is posted to a
  request list, a helper thread takes it (`FAULT_WAIT`), pulls the page from the home
  node, and completes the fault (`FAULT_DONE`). The program does `base[i]`; it never
  calls `read()`. The helper prefetches 64 pages per round trip, so one network round
  trip serves 64 faults.

Measured on 2026-10-27, two 2 GiB VMs (`krg11-1` = node 2 home, `krg11-2` = node 3
reader), segment `pool` = 65 536 pages = 256 MiB:

| | result |
|---|---|
| home: slots reserved | 65 536 (no memory taken) |
| home: pages written | 65 536, `used` 220 MB -> 477 MB (+257 MB = 256 MiB of pages) |
| reader: cache | 64 pages (256 KiB) |
| reader: read all 256 MiB, 3 runs | 32.2 / 31.3 / 35.5 MiB/s, 110-125 us/page, **0 content mismatches** |
| reader: peak resident | **64 pages = 0.2 MiB**, `used` 186 MB -> 184 MB (no growth) |
| reader: same 256 MiB through `mmap`, plain pointer loads | 6.7-7.1 s, 35.8-38.0 MiB/s, 1024 faults, **0 mismatched pages** |

Reading a page that is not resident returns `ENODATA` rather than zeros, and writing to
a cache segment returns `EPERM`: a reader's copies are read-only, so the home node can
never lose data it owns.

Batching mattered more than the network did. On a 256-page segment, one request per
page cost 1550-1970 us/page (2.0-2.5 MiB/s); 64 pages per request cost 146-150 us/page
(26.1-26.8 MiB/s) on the same two nodes. Most of the old cost was per-request overhead,
not bytes on the wire.

What does not exist: **write coherence**. The far side of a segment is read-only, so
two nodes cannot write the same pages; there is no write ownership, no invalidation,
no acquire/release. A mapped segment also cannot be evicted (the vma holds the pages),
so "read 256 MiB with a 256 KiB cache" is a property of the cached-stream API, not of
the mapping. And there is exactly one home per segment -- pages cannot be spread
across several nodes yet.

So the honest sentence is: **a process on node B can dereference memory that lives on
node A, and can stream a range much larger than its own RAM -- but the far side is
read-only and writes are not coherent.**

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
| one memory | allocate on node A, read the same bytes from node B, with a **bounded cache** on B so the test proves capacity and not just reachability; the test does not go through a file or a socket. *(reachability + capacity: passing; write-back to A: not yet)* |
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
| 3 | 一大片内存 | 在 A 节点分配的一段地址，B 节点能读能写，且 A 能看到 | 命名段 `/dev/krgdsm` 可以 `mmap`，**用指针直接读**：缺页交给一个 helper 线程，它去 home 节点把页拉回来再放行。页按需才占内存；不映射时还能给自己设**有上限的缓存**，读比自身内存大得多的段。远端只读，没有写一致性 | **部分** |
| 4 | 单一文件系统 | 同一个路径在每台机器上是同一份字节 | `krgsh ls` 在每台机器各跑一遍 `ls`，结果并排打印 | **未实现** |

**一句话**：现在是"**一个进程表 + 一个新任务的内核放置 + 一块按页拉取的共享内存**"；
`free` 里那行 "cluster" 仍然是各节点相加，那一部分是账，不是内存。

#### 内存这条到底改了什么（2026-10-27 实测，两台 2 GiB 虚拟机）

段 `pool` = 65 536 页 = 256 MiB，home 在 node 2，读端在 node 3：

- home：先只占**槽位**，写一页才占一页；写满 65 536 页后 `used` 从 220 MB 涨到 477 MB（+257 MB，即 256 MiB 页）。
- 读端：`--cache 64`（256 KiB），把 256 MiB 全文读一遍三次，32.2 / 31.3 / 35.5 MiB/s，110–125 µs/页，**内容零错**。
- 读端**峰值驻留 64 页 = 0.2 MiB**，`used` 186 MB → 184 MB（没长）。也就是说它用 256 KiB 读了 256 MiB。
- 同一个 256 MiB 换成 `mmap` 直接指针读：6.7–7.1 s，35.8–38.0 MiB/s，1024 次缺页，**内容零错**。程序里没有一次 read()。
- 读到不在本地的页返回 `ENODATA`（不是零），往缓存段写返回 `EPERM` —— 读端的副本只读，home 的数据不会被覆盖。

批量比网络更值钱：同一个 256 页的段，一页一次请求是 1550–1970 µs/页（2.0–2.5 MiB/s），
一次 64 页是 146–150 µs/页（26.1–26.8 MiB/s）。大部分开销在每次请求本身，不在线上字节。

**还没有的**：写一致性（远端只读，两个节点不能同时写同一页）、写归属、使失效、acquire/release；
映射中的段不能淘汰（页被 vma 占着），所以"256 KiB 读 256 MiB"是缓存流式接口的性质，不是 mmap 的；
一个段只能有一个 home（页还不能分散到多个节点）。

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
| 一大片内存 | 在 A 节点分配，B 节点**用指针**读同一段字节（不缺页走文件），并且能带**有上限的缓存**读比自身内存大的段；测试**不经过文件**。*(指针读 + 容量：已通过；写回对 A 可见：还没做)* |
| （单一文件系统） | **不进 v0.1**，作为已知缺口留在本文档里 |

第 4 条（单一文件系统）明确不在 v0.1 范围内，`/krg` 放在这之后做。
