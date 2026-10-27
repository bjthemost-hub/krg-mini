#!/usr/bin/env python3
"""krg-crack - a throughput workload for the cluster: SHA-256 proof-of-work search.

Find a nonce n such that sha256("<prefix><n>") starts with N hex zeros. The search
space is embarrassingly parallel and has no per-shard start-up cost, so it measures
the cluster's real hash rate rather than its coordination overhead.

  krg-crack --zeros 7 [--nodes FILE] [--workers-per-node N] [--prefix krg-mini-]

The coordinator asks every node for `nproc`, spawns one worker per core through the
kernel (`krgtool spawn`), and hands out nonce ranges from a shared queue so fast and
slow machines both stay busy. Every run carries a job id, so workers left over from
a previous run cannot report into a new one. The winning nonce is re-hashed locally
before it is printed.

Worker (spawned by the coordinator, not by a human):
  krg-crack --worker COORD_HOST COORD_PORT ZEROS PREFIX JOB
"""
import argparse
import hashlib
import itertools
import os
import socket
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

DEFAULT_NODES = os.environ.get("KRGSH_NODES", "/etc/krg-nodes.list")
KRGTOOL = os.environ.get("KRGTOOL") or (
    "/usr/local/bin/krgtool" if os.path.exists("/usr/local/bin/krgtool")
    else "/root/krg-mini/user/krgtool")
DEFAULT_PORT = 7901
CHUNK = 1000000


def read_nodes(path):
    nodes = []
    with open(path) as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 2:
                nodes.append((parts[0], parts[1]))
    return nodes


def split_addr(addr):
    host, _, port = addr.rpartition(":")
    return host, int(port or 7700)


def digest(prefix, nonce):
    return hashlib.sha256(("%s%d" % (prefix, nonce)).encode()).hexdigest()


# ------------------------------------------------------------------ worker
def worker(host, port, zeros, prefix, job):
    want = "0" * zeros

    def ask(msg):
        for _ in range(3):
            try:
                sock = socket.create_connection((host, port), timeout=15)
                sock.sendall((msg + "\n").encode())
                reply = sock.makefile().readline().strip()
                sock.close()
                return reply
            except OSError:
                time.sleep(0.5)
        return ""

    while True:
        reply = ask("GET " + job)
        if not reply or reply.startswith("STOP"):
            return 0
        parts = reply.split()
        if len(parts) != 4 or parts[0] != "RANGE" or parts[1] != job:
            return 0
        lo, hi = int(parts[2]), int(parts[3])

        for i in range(lo, hi):
            if digest(prefix, i).startswith(want):
                ask("HIT %s %d %d" % (job, i, i - lo + 1))
                return 0

        if not ask("DONE %s %d" % (job, hi - lo)):
            return 0


# ------------------------------------------------------------- coordinator
def node_cores(addr, cap):
    try:
        out = subprocess.run([KRGTOOL, "exec", addr, "nproc"], timeout=15,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        cores = int(out.stdout.decode().split()[0])
    except Exception:
        return 0
    return max(1, min(cores, cap))


def coordinator_ip(nodes):
    for _, addr in nodes:
        host, _port = split_addr(addr)
        if host in ("127.0.0.1", "localhost"):
            continue
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect((host, 7700))
            return probe.getsockname()[0]
        except OSError:
            pass
        finally:
            probe.close()
    return None


def coordinator(args):
    nodes = read_nodes(args.nodes)
    if not nodes:
        sys.exit("no nodes in %s" % args.nodes)

    plan = []
    for name, addr in nodes:
        cores = node_cores(addr, args.workers_per_node)
        if cores:
            plan.append((name, addr, cores))
        else:
            print("  %-8s unreachable" % name, file=sys.stderr)
    if not plan:
        sys.exit("no node answered")

    cip = coordinator_ip(nodes)
    if not cip:
        sys.exit("cannot determine coordinator address")

    job = "%d-%d" % (os.getpid(), int(time.time()))
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", args.port))
    ls.listen(512)
    ls.settimeout(1.0)

    lock = threading.Lock()
    counter = itertools.count(0)
    state = {"hashes": 0, "hit": None, "stop": False, "chunks": 0, "t_first": None}

    def handle(conn):
        conn.settimeout(15)
        try:
            line = conn.makefile().readline().strip()
        except OSError:
            conn.close()
            return
        reply = "STOP\n"
        parts = line.split()
        if parts and parts[0] == "GET" and len(parts) > 1 and parts[1] == job:
            with lock:
                if not state["hit"]:
                    base = next(counter) * CHUNK
                    state["chunks"] += 1
                    if state["t_first"] is None:
                        state["t_first"] = time.time()
                    reply = "RANGE %s %d %d\n" % (job, base, base + CHUNK)
        elif parts and parts[0] == "DONE" and len(parts) > 2 and parts[1] == job:
            with lock:
                state["hashes"] += int(parts[2])
        elif parts and parts[0] == "HIT" and len(parts) > 3 and parts[1] == job:
            nonce, scanned = int(parts[2]), int(parts[3])
            ok = digest(args.prefix, nonce).startswith("0" * args.zeros)
            with lock:
                state["hashes"] += scanned
                if ok and not state["hit"]:
                    state["hit"] = nonce
                elif not ok:
                    print("  ignored stale hit %d" % nonce, file=sys.stderr)
        try:
            conn.sendall(reply.encode())
        except OSError:
            pass
        conn.close()

    pool = ThreadPoolExecutor(max_workers=128)

    def serve():
        while not state["stop"]:
            try:
                conn, _ = ls.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            pool.submit(handle, conn)
    thread = threading.Thread(target=serve, daemon=True)
    thread.start()

    workers = 0
    start = time.time()
    for name, addr, cores in plan:
        cmd = ("/usr/local/bin/krg-crack --worker %s %d %d %s %s"
               % (cip, args.port, args.zeros, args.prefix, job))
        for _ in range(cores):
            try:
                subprocess.run([KRGTOOL, "spawn", addr, cmd], timeout=10,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                workers += 1
            except Exception:
                pass
    spawn_done = time.time()

    print("job %s: %d nodes, %d workers, spawn %.2f s"
          % (job, len(plan), workers, spawn_done - start), file=sys.stderr)
    for name, _addr, cores in plan:
        print("  %-8s %2d workers" % (name, cores), file=sys.stderr)

    last = time.time()
    seen = -1
    while not state["hit"]:
        time.sleep(0.1)
        if args.budget and state["hashes"] >= args.budget:
            break
        if state["hashes"] != seen:
            seen = state["hashes"]
            last = time.time()
        elif time.time() - last > 30:
            state["stop"] = True
            ls.close()
            print("no progress for 30 s (%d hashes counted) - aborted"
                  % seen, file=sys.stderr)
            print("verify     ABORTED (workers stopped reporting)")
            return 2
    elapsed = time.time() - start
    state["stop"] = True
    ls.close()

    nonce = state["hit"]
    if nonce is None:
        print("hashes     %s counted in %.2f s  => %.1f MHash/s"
              % (format(state["hashes"], ","), elapsed,
                 state["hashes"] / elapsed / 1e6))
        t_first = state["t_first"] or start
        if time.time() > t_first:
            print("compute    %s in %.2f s  => %.1f MHash/s (from first chunk)"
                  % (format(state["hashes"], ","), time.time() - t_first,
                     state["hashes"] / (time.time() - t_first) / 1e6))
        print("workers    %d on %d nodes" % (workers, len(plan)))
        print("verify     n/a (budget reached, no hit needed)")
        return 0
    hexdigest = digest(args.prefix, nonce)
    rate = state["hashes"] / elapsed / 1e6

    print("nonce      %d" % nonce)
    print("input      \"%s%d\"" % (args.prefix, nonce))
    print("sha256     %s" % hexdigest)
    t_first = state["t_first"] or start
    compute = time.time() - t_first
    print("hashes     %s counted in %.2f s  => %.1f MHash/s"
          % (format(state["hashes"], ","), elapsed, rate))
    if compute > 0:
        print("compute    %s in %.2f s  => %.1f MHash/s (from first chunk)"
              % (format(state["hashes"], ","), compute,
                 state["hashes"] / compute / 1e6))
    print("chunks     %d x %d nonces handed out" % (state["chunks"], CHUNK))
    print("workers    %d on %d nodes" % (workers, len(plan)))
    print("verify     %s" % ("OK (leading %d hex zeros)" % args.zeros
                             if hexdigest.startswith("0" * args.zeros) else "FAILED"))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worker", nargs=5,
                    metavar=("HOST", "PORT", "ZEROS", "PREFIX", "JOB"))
    ap.add_argument("--zeros", type=int, default=6)
    ap.add_argument("--nodes", default=DEFAULT_NODES)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--prefix", default="krg-mini-")
    ap.add_argument("--budget", type=int, default=0, metavar="HASHES",
                    help="stop after this many hashes instead of hunting a hit")
    ap.add_argument("--workers-per-node", type=int, default=64)
    args = ap.parse_args()

    if args.worker:
        host, port, zeros, prefix, job = args.worker
        return worker(host, int(port), int(zeros), prefix, job)
    return coordinator(args)


if __name__ == "__main__":
    sys.exit(main())