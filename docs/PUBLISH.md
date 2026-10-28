# Where to publish, and the text to paste

Everything below uses only numbers that exist in this repo with a log behind them. Do not
round them up when posting; the credibility of the piece is that it is boring and exact.

## The one table every post needs

| | one box, 40 processes | 40-node cluster | ratio |
|---|---|---|---|
| silicon | 24 cores / 48 threads (Xeon E5-2650 v4, SMT on) | 5 such hosts, 80 vCPU used | |
| work | 2.00 G SHA-256 nonces | same 2.00 G | |
| wall | 109.87 s | 40.86 s | **2.69x faster** |
| throughput | 18.2 MHash/s | 49.0 MHash/s | **2.69x** |
| per unit | 0.455 / process | 0.613 / vCPU | **1.35x** |
| kernel | 5.15.0-198 stock | 5.1.0, built here, no initramfs | |

Measured 2026-10-28, both sides the same hour, same `hashlib` loop. Sources:
`BENCHMARKS.md` (section "One box vs 40 nodes, measured the same hour") and
`REPORT_2026-10-28_5.1-kernel-40nodes.md`.

## Titles

EN:
1. `Show HN: A minimal single system image cluster on Linux 5.1 - 40 nodes vs one box`
2. `Rebuilding a Kerrighed-style SSI on Linux 5.1: 40 nodes, 2.69x one box, same kernel speed`
3. `We moved a 40-node cluster onto our own 5.1 kernel and measured what it cost (nothing)`

中文:
1. `在 Linux 5.1 上重做一个 Single System Image 集群：40 节点 vs 单机，2.69 倍`
2. `40 台虚拟机组集群算哈希：比单机 40 进程快 2.69 倍，换自制内核不掉性能`

## Body — EN (short form, for HN / Lobsters / Reddit)

> krg-mini is a small Single System Image cluster: N Linux boxes that share one process
> table, so `ps` / `kill` / `top` work across nodes, plus a shared-memory segment that is
> mmap'd and dereferenced rather than read(). It is a rewrite in the spirit of Kerrighed,
> which died around Linux 2.6.30 - we are porting the idea forward one kernel at a time,
> currently 5.1 (the last tree upstream Kerrighed ever supported), booted straight from
> GRUB with everything built in and no initramfs.
>
> Numbers from this afternoon, everything on the same five physical boxes: 40 VirtualBox
> guests, 80 vCPU, writing 2.00 G SHA-256 nonces through the cluster in 40.86 s
> (49.0 MHash/s). One of those boxes, 40 plain processes, same loop, no cluster layer:
> 109.87 s (18.2 MHash/s). So 2.69x on the same work, 1.35x per vCPU.
>
> We also swapped the kernel underneath the whole cluster from stock 5.15 to our own 5.1:
> 49.0 vs 49.2 MHash/s, i.e. nothing lost.
>
> What it is not, yet: there is no global scheduler in the kernel and no distributed shared
> memory in the kernel. `top` / `ps` / `free` on a node still see only that node; the
> cluster view is our own user-space tool. Written with an AI pair-programmer; every number
> was compiled, installed and measured on real machines, logs are in the repo.

## Body — 中文

> krg-mini 是一个小型的 Single System Image 集群：N 台 Linux 机器共享一张进程表，
> `ps` / `kill` / `top` 能跨节点用，另有一段可 mmap 后直接解引用的共享内存。
> 它是照着 Kerrighed 重写的 —— 那个项目停在 Linux 2.6.30 附近，我们一个内核一个内核地往前搬，
> 目前是 5.1（Kerrighed 上游支持过的最后一个版本），GRUB 直接引导、全内置、不带 initramfs。
>
> 今天下午在同一批五台物理机上测的：40 台虚拟机、80 vCPU，通过集群算 2.00 G 个 SHA-256
> 随机串用了 40.86 秒（49.0 MHash/s）；同一批机器里拿一台，40 个普通进程、同一条循环、
> 不带集群层，用了 109.87 秒（18.2 MHash/s）。同样工作量快 **2.69 倍**，每 vCPU 1.35 倍。
>
> 顺便把 40 台的内核从发行版 5.15 换成了我们自己编的 5.1：49.0 对 49.2 MHash/s，一点没掉。
>
> 现在还**不是**什么：内核里没有全局调度器，也没有内核态分布式共享内存；节点上的
> `top` / `ps` / `free` 仍然只看本机，集群口径是我们自己写的用户态工具。
> 和 AI 结对写的，但每个数字都是真机编译、安装、实测出来的，日志在仓库里。

## Channels

Work down the list. The first block I can do end-to-end; the rest need your account.

| channel | what it costs | notes |
|---|---|---|
| GitHub repo: topics + a Release page | nothing, I can do it | topics: `cluster`, `single-system-image`, `ssi`, `kerrighed`, `linux-kernel`, `distributed-systems`, `homelab`. Tag the 5.1 milestone as `v0.1` |
| Gitee mirror | needs your account or an access token | Chinese readers hit it much faster than GitHub; also gives a domestic issue tracker |
| Hacker News (Show HN) | needs a HN account, one shot | post EN short form, Tue-Thu morning US time works best; expect "this is just a job scheduler" comments - the honest "what it is not yet" paragraph is the answer |
| Lobsters | account | same text, tag `linux` + `distributed`; smaller and more technical than HN |
| Reddit r/linux, r/selfhosted, r/homelab | account, and mind each sub's self-promo rules | r/linux tolerates original work with numbers; r/homelab wants the "I built this at home" framing, not a paper |
| Phoronix Forums | account | the kernel/benchmark crowd; post the kernel-swap table, that is the part they will poke at |
| LWN | comment needs a subscription | read-only otherwise; do not paste the whole write-up, link it |
| V2EX (分享创造 / 程序员) | account | 中文正文；V2EX 反广告，标题别带"震惊"类 |
| 知乎 | account, long-form | 中文正文 + 那张对比表；适合留一份可被搜索到的长文 |
| 掘金 / 开源中国 OSChina / 博客园 / CSDN | account | 同一份中文稿分发；OSChina 顺带把仓库挂上去 |
| B 站 / 公众号 | account, needs a video or a nicer layout | 用 `krg-top` 那段 40 卡片跑满的画面做十几秒动图，比纯文字有效 |

Rules for every channel:

- **Never claim the stock `top` / `ps` / `free` are cluster-wide.** They are not; ours are.
- **Never claim kernel-level DSM or a kernel scheduler.** Neither exists yet; the write-up says
  so in the same paragraph as the numbers.
- Keep the ratio at 2.69x for the same-hour pair, and say "one box, 40 processes on 24 cores"
  rather than "40-core box" - we do not have a 40-core machine and someone will ask.
- Say AI-assisted in the first screen of text, not in a footnote.
