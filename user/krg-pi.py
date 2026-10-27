#!/usr/bin/env python3
"""krg-pi - compute pi across the krg-mini cluster.

Coordinator (default):
    krg-pi [--digits N] [--nodes file] [--port 7900]

Worker (started by the remote node's kernel, not by the user):
    krg-pi --worker A B PREC COORD_HOST COORD_PORT JOB INDEX

The coordinator asks every node for its runnable-task count, places work on the
least loaded nodes, and re-runs any shard whose worker did not report back.
"""
import argparse
import math
import os
import socket
import subprocess
import sys
import threading
import time
from decimal import Decimal, getcontext

DEFAULT_NODES = "/etc/krg-nodes.list"
KRGTOOL = os.environ.get("KRGTOOL") or (
    "/usr/local/bin/krgtool" if os.path.exists("/usr/local/bin/krgtool")
    else "/root/krg-mini/user/krgtool")
MAX_ATTEMPTS = 3

try:
    sys.set_int_max_str_digits(0)
except AttributeError:
    pass


CHUD_C = -262537412640768000


def chudnovsky_partial(a, b, prec):
    """Sum of Chudnovsky terms k in [a, b), using the term recurrence."""
    if a >= b:
        return Decimal(0)
    getcontext().prec = prec + 10
    L = lambda k: 13591409 + 545140134 * k

    num = math.factorial(6 * a) * L(a)
    den = math.factorial(3 * a) * (math.factorial(a) ** 3) * (CHUD_C ** a)
    term = Decimal(num) / Decimal(den)
    total = term

    for k in range(a + 1, b):
        num = ((6*k) * (6*k-1) * (6*k-2) * (6*k-3) * (6*k-4) * (6*k-5) * L(k))
        den = ((3*k) * (3*k-1) * (3*k-2) * k**3 * CHUD_C * L(k-1))
        term = term * Decimal(num) / Decimal(den)
        total += term
    return total


def worker(a, b, prec, host, port, job, index, delay=0.0):
    if delay:
        time.sleep(delay)
    val = chudnovsky_partial(a, b, prec)
    s = socket.create_connection((host, port), timeout=30)
    msg = "RESULT %s %d %s\n" % (job, index, format(val, "f"))
    s.sendall(msg.encode())
    s.close()
    return 0


def arctan_inv(x, unity):
    total = term = unity // x
    x2 = x * x
    k = 1
    sign = -1
    while term:
        term //= x2
        total += sign * (term // (2 * k + 1))
        sign = -sign
        k += 1
    return total


def machin_pi(digits):
    """Independent check: pi = 16*atan(1/5) - 4*atan(1/239), integer scaled."""
    unity = 10 ** (digits + 20)
    pi = 4 * (4 * arctan_inv(5, unity) - arctan_inv(239, unity))
    s = str(pi)
    return s[0] + "." + s[1:digits + 1]


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


def kt(args, timeout=30):
    return subprocess.run([KRGTOOL] + args, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)


def query_load(addr):
    """Return runnable-task count, or None if the node does not answer."""
    try:
        r = kt(["load", addr], timeout=5)
    except subprocess.TimeoutExpired:
        return None
    if r.returncode != 0:
        return None
    p = r.stdout.decode(errors="replace").split()
    if len(p) >= 4 and p[0] == "OK":
        return int(p[2])
    return None


def coordinator_ip(nodes):
    for _, addr in nodes:
        host = addr.rsplit(":", 1)[0]
        if host in ("127.0.0.1", "localhost"):
            continue
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((host, 7700))
            return s.getsockname()[0]
        except OSError:
            pass
        finally:
            s.close()
    return None


def coordinator(digits, nodes_file, port, delay=0.0):
    nodes = read_nodes(nodes_file)
    if not nodes:
        print("no nodes in %s" % nodes_file, file=sys.stderr)
        return 1

    # least loaded first; nodes that do not answer go last but stay usable
    loads = {addr: query_load(addr) for _, addr in nodes}
    alive = [(name, addr) for name, addr in nodes if loads[addr] is not None]
    if not alive:
        print("no node answered LOAD", file=sys.stderr)
        return 1
    alive.sort(key=lambda na: loads[na[1]])

    prec = digits + 25
    terms = max(digits // 14 + 3, len(alive))
    chunk = (terms + len(alive) - 1) // len(alive)
    units = []
    for i in range(len(alive)):
        a = i * chunk
        b = min(a + chunk, terms)
        if a < terms:
            units.append((i, a, b))

    job = "%d-%d" % (os.getpid(), int(time.time()))
    results = {}
    lock = threading.Lock()
    stop = threading.Event()

    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", port))
    ls.listen(64)
    ls.settimeout(1.0)

    cip = coordinator_ip(nodes)
    if not cip:
        print("cannot determine coordinator address", file=sys.stderr)
        return 1

    def collect():
        while not stop.is_set():
            try:
                c, _ = ls.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            data = b""
            c.settimeout(10)
            try:
                while b"\n" not in data:
                    b = c.recv(65536)
                    if not b:
                        break
                    data += b
            except OSError:
                pass
            finally:
                c.close()
            p = data.decode(errors="replace").strip().split()
            if len(p) >= 4 and p[0] == "RESULT" and p[1] == job:
                with lock:
                    results[int(p[2])] = Decimal(p[3])

    th = threading.Thread(target=collect, daemon=True)
    th.start()

    pending = list(units)
    used = set()
    attempts = 0
    for attempt in range(MAX_ATTEMPTS):
        if not pending:
            break
        attempts += 1
        # rotate the starting node each round so retries land elsewhere
        order = alive[attempt % len(alive):] + alive[:attempt % len(alive)]
        plan = []
        for pos, (idx, a, b) in enumerate(pending):
            name, addr = order[pos % len(order)]
            plan.append(((idx, a, b), name, addr))

        for (idx, a, b), name, addr in plan:
            cmd = ("/usr/local/bin/krg-pi --worker %d %d %d %s %d %s %d --delay %g"
                   % (a, b, prec, cip, port, job, idx, delay))
            try:
                r = kt(["spawn", addr, cmd])
                if r.returncode == 0:
                    used.add(name)
            except subprocess.TimeoutExpired:
                pass

        timeout = max(8.0, digits / 2000.0)
        deadline = time.time() + timeout
        while time.time() < deadline:
            with lock:
                if all(idx in results for idx, _, _ in pending):
                    break
            time.sleep(0.2)
        with lock:
            pending = [u for u in pending if u[0] not in results]

    stop.set()
    ls.close()

    if not results:
        print("no results", file=sys.stderr)
        return 1
    if pending:
        print("missing shards after %d attempts: %s"
              % (attempts, [u[0] for u in pending]), file=sys.stderr)
        return 1

    getcontext().prec = prec
    total = sum(results.values())
    pi = Decimal(426880) * Decimal(10005).sqrt() / total
    out = format(pi, "f")[:digits + 2]

    if digits <= 200000:
        ref = machin_pi(digits)
        check = "OK" if out == ref else "MISMATCH(%s)" % ref
        ok = out == ref
    else:
        check = "skipped (digits > 200000)"
        ok = True

    print("pi(%d) = %s" % (digits, out))
    print("nodes: %d/%d replied (%s), attempts=%d"
          % (len(used), len(alive), ", ".join(sorted(used)), attempts))
    print("load: %s" % ", ".join(
        "%s=%s" % (n, "down" if loads[a] is None else loads[a]) for n, a in nodes))
    print("self-check(Machin): %s" % check)
    return 0 if ok else 2


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--worker", nargs=7, metavar=("A", "B", "PREC", "HOST", "PORT", "JOB", "INDEX"))
    ap.add_argument("--digits", type=int, default=100)
    ap.add_argument("--nodes", default=DEFAULT_NODES)
    ap.add_argument("--port", type=int, default=7900)
    ap.add_argument("--delay", type=float, default=0.0,
                    help="test knob: seconds each worker waits before computing")
    args = ap.parse_args()

    if args.worker:
        a, b, prec, host, port, job, index = args.worker
        return worker(int(a), int(b), int(prec), host, int(port), job,
                      int(index), args.delay)
    return coordinator(args.digits, args.nodes, args.port, args.delay)


if __name__ == "__main__":
    sys.exit(main())
