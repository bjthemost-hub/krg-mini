# Benchmarks

All numbers below were measured on 2026-10-27, on the same cluster and the same
code, with wall-clock timing (`date +%s%N` around the process). They are the whole
process: Python start-up, node discovery, fan-out, compute and the self-check.

## Environment

| item | head node | worker |
|---|---|---|
| machine | physical, Intel Xeon E5-2650 v4 @ 2.20 GHz | VirtualBox 6.1.50 on the same host (`systemd-detect-virt` = oracle) |
| CPUs | 48 (`nproc`) | 2 |
| memory | 65,830,520 kB total (62.8 GiB) | 2,010,784 kB (1963 MiB) |
| OS / kernel | Ubuntu 22.04.5 LTS, 5.15.0-198-generic | same image, same kernel |
| python | 3.10.12 | 3.10.12 |

Cluster: 13 nodes — 1 physical + 12 VMs — each running `krgtool serve` on 7700,
`node_id` 1..13, LAN latency (same switch, VMs NATed on the host).

## pi(100) — what the question usually is

`krg-pi --digits 100`, 5 runs each, milliseconds:

| setup | runs | median |
|---|---|---|
| 13 nodes (13 shards, one per node) | 330 / 305 / 308 / 307 / 302 | **307** |
| one VM, 1 shard | 264 / 264 / 262 / 263 / 263 | **263** |
| one VM, 13 shards (same split, forced onto one box) | 656 / 637 / 645 / 653 / 678 | **653** |
| in-process, no cluster (13 terms + sqrt) | 0.1 – 0.2 | — |

Read this honestly: 100 digits of pi is **8 terms of Chudnovsky**, i.e. 0.1–0.2 ms
of arithmetic. The cluster is 44 ms *slower* than one VM because it does 13 `LOAD`
probes and 13 `SPAWN` calls instead of 1+1. pi(100) is a correctness test, not a
benchmark.

## pi(50000) — where it should pay off, and does not

1 run each:

| setup | wall |
|---|---|
| 13 nodes | 7915 ms |
| one VM, 1 shard | 7090 ms |
| one VM, 13 shards | 11701 ms |

Breakdown measured on the head node, single process, same precision:

| piece | time |
|---|---|
| Chudnovsky terms k=0..275 | 106 ms |
| terms k=275..550 | 128 ms |
| terms k=3296..3571 (last shard, includes its start-up term) | 2264 ms |
| `machin_pi(50000)` self-check | 4803 ms |
| one `krgtool load` round trip | 1 ms |

So ~4.8 s of every run is the serial Machin check, and the remaining spread is
dominated by each shard computing its own start-up term
(`factorial(6a)`, `CHUD_C ** a`) before the recurrence can begin — the larger `a`,
the worse. Parallelism is real but eaten by that start-up cost.

**Known fix, not implemented**: precompute the start terms (or switch to binary
splitting) so a shard costs O(terms) instead of O(start index), and move the
self-check off the critical path.

## Reproducing

```sh
# cluster
time krg-pi --digits 100
time krg-pi --digits 50000

# single node: same code, one line in the node list
printf 'work1 10.0.7.21:7700\n' > /tmp/one.list
time krg-pi --digits 100 --nodes /tmp/one.list

# one node forced to run every shard: repeat the same line 13 times
for i in $(seq 13); do echo 'work1 10.0.7.21:7700'; done > /tmp/thirteen.list
time krg-pi --digits 100 --nodes /tmp/thirteen.list
```

## Cluster shell cost

`krgsh` fans out one request per node over a thread pool; with 13 nodes on a LAN a
merged `top`, `free`, `ls` or `bash` returns in one round trip (~0.5-0.7 s wall
including Python start-up and 13 TCP connections, measured on the same cluster).
## Throughput: SHA-256 search (2026-10-27, 15 nodes, fixed work)

`krg-crack` is a second workload beside `krg-pi`: find a nonce so that
`sha256("<prefix><n>")` starts with N hex zeros. The search space is cut into
1,000,000-nonce chunks, workers pull chunks from the coordinator, and the winner is
re-hashed locally. Unlike `krg-pi` (whose shards pay a per-shard start-up term and a
serial self-check), this one is pure throughput, so it measures the cluster rather
than the algorithm.

Measurements below use `--budget <hashes>`: the coordinator stops after a fixed
number of hashes and reports the rate, so a lucky early hit cannot flatter the
number. `--zeros 12` keeps a hit from ever interfering. Wall clock is measured by
the process itself, from before node discovery to the last chunk.

| rig | threads | work | wall | rate | per thread |
|---|---|---|---|---|---|
| one VM, 1 worker | 1 | 30 M | 45.86 s | 0.7 MHash/s | 0.70 |
| one VM, 2 workers | 2 | 60 M | 45.57 s | 1.3 MHash/s | 0.65 |
| 12 VMs (24 vCPU, 3 hosts) | 24 | 601 M | 40.34 s | 14.9 MHash/s | 0.62 |
| one 48-thread server (A) | 48 | 1.00 G | 69.07 s | 14.5 MHash/s | 0.30 |
| one 48-thread server (B) | 48 | 1.00 G | 85.41 s | 11.7 MHash/s | 0.24 |
| **15-node cluster** | **168** | **2.00 G** | **46.29 s** | **43.3 MHash/s** | **0.26** |

Same loop without the cluster layer (`raw_sha.py`: `fork()` N children, each hashing
straight from `hashlib`, same 100,000-hash inner chunk):

| machine | processes | rate | per process |
|---|---|---|---|
| server A (2x12 cores, SMT on, 48 threads) | 1 | 0.79 MHash/s | 0.79 |
| server A | 16 | 12.9 MHash/s | 0.81 |
| server A | 48 | 18.3 MHash/s | 0.38 |
| server B (same CPU) | 1 | 0.83 MHash/s | 0.83 |
| server B | 48 | 16.9 MHash/s | 0.35 |
| one VM (2 vCPU) | 1 | 0.73 MHash/s | 0.73 |
| one VM | 2 | 1.6 MHash/s | 0.79 |

What this says, stated narrowly:

- **The cluster does scale.** 43.3 MHash/s is 2.4x what one of the 48-thread servers
  manages *without* the cluster layer (18.3), and 33x one 2-vCPU VM (1.3).
- **The win comes from core count, not from the SSI.** Per thread the cluster is
  slower than bare metal: 0.26 vs 0.38 MHash/s at 48 processes, i.e. ~68%. The
  coordinator is a single Python process doing a socket round trip per 1M-hash
  chunk, and the head node also hosts four VMs, so it is oversubscribed while it
  coordinates.
- **24 VM threads ~ one 48-thread server** (14.9 vs 14.5). The 48 "threads" are
  24 physical cores with SMT, and SHA-256 in CPython gains almost nothing from the
  sibling thread. Spread one worker per physical core and the two rigs land on the
  same number.
- **Fan-out is not free but it is small**: spawning 168 workers across 15 nodes
  takes 4.9 s (5.41 / 4.93 / 7.57 s over three runs), out of a 46 s run.

Time to actually answer an 8-hex-zero preimage (expected 16^8 = 4.29 G hashes).
The "expected" column is arithmetic from the measured rate, not a separate run:

| rig | rate | expected time |
|---|---|---|
| one VM, 2 vCPU | 1.3 MHash/s | ~55 min |
| one 48-thread server | 14.5 MHash/s | ~5 min |
| 12 VMs | 14.9 MHash/s | ~5 min |
| 15-node cluster | 43.3 MHash/s | ~1.6 min |

Three real hit-finding runs, kept because they show the variance rather than hide
it: the cluster found 8 zeros after 455 M hashes in 13.2 s, and another time only
after 8.21 G hashes in 179 s; server B after 573 M in 39.7 s; server A ran 5.67 G
in 6 m 49 s without a hit and was aborted. A single random search is a bad timer —
use the fixed-budget rates above, and treat these as "it does finish".

## Where the coordinator runs (2026-10-27, 15 nodes, 2 G hashes each)

Same cluster, same `--zeros 12 --budget 2000000000 --workers-per-node 64`. The
only variable is which machine runs the coordinator. Three runs each:

| coordinator | runs (MHash/s) | median | fan-out (spawn) |
|---|---|---|---|
| a worker node, and the host of four VMs | 43.3 / 43.7 / 44.0 | **43.7** | 4.53 / 4.93 / 6.88 s |
| a separate ARM box (Jetson Orin, 12 cores, coordinator only) | 49.2 / 46.2 / 49.3 | **49.2** | 1.48 / 2.84 s |

Moving the coordinator off the busy machine is worth **+12.6%** (43.7 -> 49.2
MHash/s) and cuts fan-out from ~4.9 s to ~2 s, even though that box's single core
is slower than one Xeon core. The coordinator hashes nothing; what it needs is
not being preempted, not raw single-core speed.

This also qualifies the 0.26-per-thread row above: most of that gap is the head
node competing with its own four VMs, not the cluster layer. With a dedicated
coordinator the cluster runs at 0.29 MHash/s per thread against 0.38 on bare
metal (~77%), so 168 threads reach 49.2 MHash/s — 2.7x one 48-thread server with
no cluster layer at all.
Environment: 3 physical servers (2x12-core Xeon E5-2650 v4, 48 threads, 62.8 GiB
each; one of them 31.3 GiB), 12 VirtualBox VMs of 2 vCPU / 1.9 GiB spread over
three of the servers, Ubuntu 22.04.5, kernels 5.15.0-119/131/198. One server per
rig means the VMs and the head node share silicon on the 12-VM and cluster runs.

## Throughput: 20 nodes, 5 hosts x 4 VMs (2026-10-27)

The 15-node rig above mixes three physical servers with 12 VMs, and the servers were
*themselves* cluster nodes — 168 workers on 144 physical threads. So its per-thread
figure (0.26) mostly measured oversubscription, not the cluster layer. The rig below
is the topology the project now runs on: **every node is a VM, physical boxes only
host them.**

| item | value |
|---|---|
| nodes | 20 VMs, 4 per host, spread over 5 hosts |
| per node | 2 vCPU, 2 GiB, Ubuntu 22.04.5, kernel 5.15.0-198 |
| total vCPU | 40 |
| coordinator | a separate ARM box (12 cores) that hashes nothing and is not a node |
| workload | `krg-crack --zeros 12 --budget 2000000000`, 2 workers per node |

### How the rig is built

Five identical 2x12-core servers, each running four VirtualBox guests (2 vCPU, 2 GiB,
one shared Ubuntu image). Every guest gets its own `node_id` via `deploy/install.sh`,
`krg-mini.service` starts `krgtool serve 7700`, the host maps four local ports to the
four guests, and all 20 nodes carry the same 20-line `/etc/krg-nodes.list`.

The hosts are **not** nodes: their own `krg-mini` is stopped and disabled, so the
whole compute budget is 40 vCPU and no host is oversubscribed (8 vCPU on a 48-thread
server). The coordinator is a separate 12-core ARM board on the same switch — it
holds the node list and the chunk queue, and hashes nothing.

Three runs, same 2 G of work each (the coordinator counts in 1M-nonce chunks, so the
last chunk can overshoot by up to one chunk):

| run | counted | wall | rate | settle |
|---|---|---|---|---|
| 1 | 2,000,000,000 | 78.23 s | 25.6 MHash/s | 0.39 s |
| 2 | 2,000,000,000 | 78.78 s | 25.4 MHash/s | 0.64 s |
| 3 | 2,001,000,000 | 78.71 s | 25.4 MHash/s | 0.44 s |
| **median** | | | **25.4 MHash/s** | |

Per thread that is 0.635 MHash/s (25.4 / 40).

### Same loop, no cluster layer

`raw_sha.py` on one of the hosts (2x12-core Xeon E5-2650 v4, SMT on, 48 threads):

| processes | work | wall | rate | per process |
|---|---|---|---|---|
| 8 | 400 M | 61.25 s | 6.5 MHash/s | 0.816 |
| 20 | 800 M | 49.22 s | 16.3 MHash/s | 0.813 |
| 40 | 800 M | 42.20 s | 19.0 MHash/s | 0.474 |

### What the numbers say

- **1.56x a single 20-core box**: 25.4 MHash/s against 16.3 MHash/s for 20 bare
  processes on one server. The cluster's 40 vCPU are spread 8 per host, so no host
  is oversubscribed.
- **1.34x a single box running 40 processes** (19.0): past 24 physical cores the box
  is fighting SMT, per process drops from 0.81 to 0.47, while the cluster still gets
  0.635 per vCPU.
- **78% per-thread efficiency** (0.635 vs 0.813 at 20 bare processes). The missing
  fifth is the coordinator: one Python process, one socket round trip per 1M-hash
  chunk, plus 20 `exec nproc` probes and 40 `spawn` calls at start.
- **Fan-out is cheap here**: 0.39 / 0.64 / 0.44 s to start 40 workers on 20 nodes,
  out of a 78 s run — under 1%. On the 15-node rig it was 4.9 s of 46 s, because
  that coordinator was itself a worker on an oversubscribed host.
- **The 15-node 49.2 MHash/s is not comparable**: it is 168 threads, this is 40.
  Per thread 0.29 vs 0.635 — the difference is the topology, not the code.

![20-node cluster at full load: 20/20 nodes online, 40 cores at 99.8% CPU,
40 workers](images/krg-top-20nodes.png)

`krg-top` during run 2. Every card is one VM (2 cores at ~100%), `workers 40` is one
`krg-crack` worker per vCPU, the header line is the whole-cluster average. The board
is served by the head node, reads every node through `krgtool exec`, and shows a node
as unreachable when it cannot be reached.

### Reproducing

```sh
# 20-node list: "name host:7700", one line per VM
wc -l /etc/krg-nodes.list          # 20

# fixed amount of work, so a lucky hit cannot flatter the number
krg-crack --zeros 12 --budget 2000000000 --nodes /etc/krg-nodes.list --workers-per-node 2

# same inner loop, no cluster layer, on one of the hosts
python3 raw_sha.py 20 40000000     # 20 processes, 800 M hashes total
```

The screenshot needs no scrubbing: node names are `<host>-<slot>`, and the page shows
no addresses. Everything else in this document is the same — no hostnames, no IPs,
no credentials.