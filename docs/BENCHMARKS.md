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