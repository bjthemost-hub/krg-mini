# Design

Everything here is visible in ~1100 lines of code; this file is the map.

## 1. Global pid

```c
gpid = ((node_id & 0xff) << 24) | (pid & 0x00ffffff)
```

`node_id` is a module parameter (`/etc/modprobe.d/krg-node.conf`), unique per
machine, 1..255. That is why `krgtool kill <peer> <gpid>` can sanity-check that the
pid belongs to the peer it is talking to instead of killing a local task with the
same number.

## 2. `/dev/krgview` — the kernel side

A misc device, mode 0400, one `read()` returns everything:

```
struct krgv_hdr {            // 32 bytes
    __u32 magic;             // 0x4b524779 ("KRGy", v4)
    __u32 count;             // records that follow
    __u32 max;               // capacity (KRGV_MAX_TASKS = 4096)
    __u32 node_id;
    __u32 run;               // tasks in TASK_RUNNING (load signal)
    __u32 pad;
    __u32 mem_total_kb;      // si_meminfo()->totalram  << (PAGE_SHIFT - 10)
    __u32 mem_free_kb;       // si_meminfo()->freeram   << (PAGE_SHIFT - 10)
};
struct krgv_task {           // 40 bytes each
    __u32 pid, tgid, ppid, state, rss_kb, gpid;
    char  comm[16];
};
```

The table is produced under `rcu_read_lock()` with `for_each_process()` +
`get_task_comm()` + `get_task_mm()`; it is thread-level, not deduplicated by tgid.

`write()` takes one of:

- `SPAWN <shell command>` — `call_usermodehelper("/bin/sh", "-c", cmd)`, i.e. the
  **kernel** creates the task on this node (no ssh, no agent).
- `KILL <gpid>` — only accepted if the gpid's node byte is this node; then
  `kill_pid(find_get_pid(pid))`.

## 3. `krgtool serve` — the wire protocol

One request line, replies as below. This is deliberately line-oriented so that
`nc` or a 30-line script can talk to it.

| request | reply |
|---|---|
| `PING` | `PONG` |
| `LIST` | optional `# load <run> <tasks>`, optional `# mem <total_kb> <free_kb>`, then `<pid> <ppid> <st> <tgid> <rss_kb> <gpid> <comm>` per task, then `END` |
| `LOAD` | `OK <node_id> <run> <tasks>` or `ERR` |
| `MEM` | `OK <node_id> <total_kb> <free_kb>` or `ERR` |
| `EXEC <cmd>` | raw stdout/stderr of `popen(cmd)` then `END` |
| `SPAWN <cmd>` / `KILL <gpid>` | `OK` or `ERR <errno>` |

`EXEC` is userspace on purpose: `free`/`top`/`ls`/`bash` need a shell, not a kernel
thread. `SPAWN` stays in the kernel because creating a task without a daemon is the
part that makes this an SSI experiment rather than an ssh wrapper.

## 4. `krgsh` — the cluster shell

`/etc/krg-nodes.list` (`<name> <host:port>`, one per line, `#` comments) is read at
startup; `KRGSH_NODES` overrides the path.

Each built-in turns into one protocol request fanned out over a thread pool with
`max_workers = number of nodes`, and the results are printed in node-list order so
output is stable:

| built-in | request | merge rule |
|---|---|---|
| `free` | `MEM` | sum totals/free over all nodes, one aggregate line + one line per node sorted by used% |
| `top [-n N] [-r]` | `LIST` | concatenate rows, sort by RSS, keep N; `-r` prints each node's `run`/`tasks` |
| `ls [args]` | `EXEC ls ...` | one labelled block per node |
| `bash <cmd>` | `EXEC <cmd>` | one labelled block per node |

Nodes that do not answer are reported once as `[unreachable: ...]` and do not
abort the command. Unknown words are suggested as `bash "<line>"`.

## 5. `krg-pi` — placement and retry

1. `LOAD` every node (in sequence, ~1 ms each on a LAN), sort by `run`.
2. `terms = max(digits/14 + 3, len(nodes))`, split into equal term ranges — one
   shard per node when `digits` is small.
3. For each shard: `SPAWN /usr/local/bin/krg-pi --worker A B PREC COORD_IP PORT JOB IDX`
   with a 1 s accept timeout on the coordinator.
4. Workers push `RESULT <job> <idx> <decimal>` back to the coordinator's TCP port
   (7900 by default).
5. Shards that miss the deadline are re-planned onto other nodes; up to 3 rounds.
6. The sum is turned into pi and checked against an independent Machin
   computation (`16*atan(1/5) - 4*atan(1/239)` in integers) before printing.

## 6. What is missing (and why it matters)

- **Process migration**: the whole point of Kerrighed, and a rewrite of scheduler,
  memory and fs state. Not attempted.
- **Distributed shared memory**: krg-mini shares nothing but the process table, so
  `ls` on each node shows that node's own filesystem.
- **Global scheduling**: placement is decided by the coordinator in userspace.
- **Authentication**: none. Port 7700 executes commands for anyone who can reach it.