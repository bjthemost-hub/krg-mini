#!/usr/bin/env python3
"""krgsh - the krg-mini cluster shell.

Built-in commands run on every node listed in /etc/krg-nodes.list and merge the
results, so a single terminal sees the whole cluster as one machine:

    free                 cluster memory: one total line + one line per node
    top [-n N] [-r]      merged task table from all nodes, heaviest RSS first
    ls [args] [path]     run ls with the same arguments on every node
    bash <command>       run a shell command on every node

Other built-ins: nodes, show (task table by node), exit/quit, help.
One-shot use:  krgsh -c "free"
"""
import os
import shlex
import socket
import sys
import concurrent.futures as futures

DEFAULT_NODES = os.environ.get("KRGSH_NODES", "/etc/krg-nodes.list")
TIMEOUT = 60


def read_nodes(path=None):
    path = path or DEFAULT_NODES
    nodes = []
    try:
        with open(path) as fp:
            for line in fp:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) >= 2:
                    nodes.append((parts[0], parts[1]))
    except OSError as exc:
        sys.exit("krgsh: cannot read %s: %s" % (path, exc))
    return nodes


def split_addr(addr):
    host, _, port = addr.rpartition(":")
    return host or "127.0.0.1", int(port or 7700)


def send(addr, request, timeout=TIMEOUT):
    """Send one request line, return the whole reply as text."""
    host, port = split_addr(addr)
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(request.encode())
        chunks = []
        while True:
            data = sock.recv(65536)
            if not data:
                break
            chunks.append(data)
            if b"END\n" in data or data.startswith((b"OK ", b"ERR")):
                # single-line replies (OK/ERR) are complete after one packet;
                # LIST/EXEC streams end with END
                if not request.startswith(("LIST", "EXEC")):
                    break
    return b"".join(chunks).decode(errors="replace")


def fan_out(nodes, request, timeout=TIMEOUT):
    """Run one request against every node in parallel.

    Returns {name: (addr, text_or_None, error_or_None)} in node-list order.
    """
    def one(item):
        name, addr = item
        try:
            return name, addr, send(addr, request, timeout), None
        except Exception as exc:                      # noqa: BLE001
            return name, addr, None, str(exc)

    out = {}
    with futures.ThreadPoolExecutor(max_workers=max(1, len(nodes))) as pool:
        for name, addr, text, err in pool.map(one, nodes):
            out[name] = (addr, text, err)
    return out


def strip_end(text):
    if text is None:
        return None
    if text.endswith("END\n"):
        text = text[:-4]
    elif text.endswith("END"):
        text = text[:-3]
    return text


def meminfo(node_addr):
    """Return (node_id, total_kb, free_kb) or None."""
    reply = send(node_addr, "MEM\n", timeout=5)
    parts = reply.split()
    if len(parts) >= 4 and parts[0] == "OK":
        return int(parts[1]), int(parts[2]), int(parts[3])
    return None


def gib(kb):
    return kb / (1024.0 * 1024.0)


# ---------------------------------------------------------------- built-ins
def cmd_free(nodes, args):
    rows, down = [], []
    with futures.ThreadPoolExecutor(max_workers=max(1, len(nodes))) as pool:
        futs = {pool.submit(meminfo, addr): (name, addr) for name, addr in nodes}
        got = {}
        for fut in futures.as_completed(futs):
            name, addr = futs[fut]
            try:
                res = fut.result()
            except Exception:                          # noqa: BLE001
                res = None
            if res is None:
                down.append(name)
            else:
                got[name] = res
    for name, addr in nodes:
        if name in got:
            rows.append((name,) + got[name])

    total = sum(r[2] for r in rows)
    free = sum(r[3] for r in rows)
    used = total - free
    pct = (100.0 * used / total) if total else 0.0
    print("%-10s %10s %10s %10s %6s" %
          ("", "total", "used", "free", "used%"))
    print("%-10s %8.1fGi %8.1fGi %8.1fGi %5.1f%%" %
          ("cluster", gib(total), gib(used), gib(free), pct))
    print()
    print("%-10s %8s %10s %10s %10s %6s" %
          ("NODE", "id", "total_MiB", "used_MiB", "free_MiB", "used%"))
    for name, nid, tot, fre in sorted(rows, key=lambda r: (r[3] / r[2]) if r[2] else 0):
        u = tot - fre
        p = (100.0 * u / tot) if tot else 0.0
        print("%-10s %8d %10d %10d %10d %5.1f%%" %
              (name, nid, tot // 1024, u // 1024, fre // 1024, p))
    if down:
        print("\n[unreachable: %s]" % ", ".join(down))


def cmd_top(nodes, args):
    limit = 15
    show_run = False
    i = 0
    while i < len(args):
        if args[i] == "-n" and i + 1 < len(args):
            limit = int(args[i + 1])
            i += 2
        elif args[i] == "-r":
            show_run = True
            i += 1
        else:
            i += 1

    replies = fan_out(nodes, "LIST\n")
    rows, load, down = [], {}, []
    for name, _addr in nodes:
        text, err = replies[name][1], replies[name][2]
        if err or text is None:
            down.append(name)
            continue
        for line in strip_end(text).splitlines():
            if line.startswith("# load "):
                p = line.split()
                load[name] = (int(p[2]), int(p[3]))
                continue
            if not line or line.startswith("#"):
                continue
            p = line.split(None, 6)
            if len(p) < 7:
                continue
            pid, ppid, st, tgid, rss, gpid, comm = p
            rows.append((name, int(gpid), int(pid), int(ppid), st,
                         int(tgid), int(rss), comm))

    if show_run:
        print("%-10s %8s %8s" % ("NODE", "run", "tasks"))
        for name, _addr in nodes:
            if name in load:
                print("%-10s %8d %8d" % (name, load[name][0], load[name][1]))
            else:
                print("%-10s %8s %8s" % (name, "-", "-"))
        print()

    rows.sort(key=lambda r: r[6], reverse=True)
    print("%-10s %8s %7s %7s %-2s %8s %8s  %s" %
          ("NODE", "GPID", "PID", "PPID", "ST", "TGID", "RSS_KB", "COMM"))
    for r in rows[:limit]:
        print("%-10s %8d %7d %7d %-2s %8d %8d  %s" %
              (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]))
    print("\n%d tasks on %d nodes, showing top %d by RSS" %
          (len(rows), len(nodes) - len(down), min(limit, len(rows))))
    if down:
        print("[unreachable: %s]" % ", ".join(down))


def cmd_run_all(nodes, command, title):
    """Run `command` everywhere and print each node's output in node order."""
    replies = fan_out(nodes, "EXEC %s\n" % command)
    down = []
    for name, addr in nodes:
        _a, text, err = replies[name]
        print("=== %s (%s) ===" % (name, addr))
        if err or text is None:
            down.append(name)
            print("!! %s" % err)
            continue
        out = strip_end(text).rstrip("\n")
        print(out if out else "(no output)")
    if down:
        print("\n[unreachable: %s]" % ", ".join(down))
    return 0 if not down else 1


def cmd_ls(nodes, args):
    if not args:
        args = ["-1"]
    return cmd_run_all(nodes, "ls " + " ".join(shlex.quote(a) for a in args), "ls")


def cmd_bash(nodes, args):
    if not args:
        print("usage: bash <command>   (runs on every node)")
        return 2
    if args[0] in ("-c", "--command") and len(args) > 1:
        args = args[1:]
    return cmd_run_all(nodes, " ".join(args), "bash")


def cmd_nodes(nodes, args):
    print("%-10s %s" % ("NODE", "ADDRESS"))
    for name, addr in nodes:
        try:
            reply = send(addr, "PING\n", timeout=3).strip()
        except Exception as exc:                       # noqa: BLE001
            reply = "DOWN (%s)" % exc
        print("%-10s %-22s %s" % (name, addr, reply))
    return 0


def cmd_show(nodes, args):
    return cmd_top(nodes, ["-n", "1000000"])


BUILTINS = {
    "free": cmd_free,
    "top": cmd_top,
    "ls": cmd_ls,
    "bash": cmd_bash,
    "sh": cmd_bash,
    "nodes": cmd_nodes,
    "show": cmd_show,
}

HELP = """krgsh - cluster shell over %d nodes (/etc/krg-nodes.list)

  free                  cluster memory, aggregate + per node
  top [-n N] [-r]       merged task table (top N by RSS; -r shows run queue)
  ls [args] [path]      same ls arguments on every node
  bash <command>        run a shell command on every node
  nodes                 ping every node
  show                  full merged task table
  help                  this text
  exit | quit | Ctrl-D   leave
"""


def run_line(line, nodes):
    try:
        args = shlex.split(line)
    except ValueError as exc:
        print("krgsh: %s" % exc)
        return 1
    if not args:
        return 0
    name, rest = args[0], args[1:]
    if name in ("exit", "quit"):
        raise SystemExit(0)
    if name in ("help", "-h", "--help"):
        print(HELP % len(nodes))
        return 0
    fn = BUILTINS.get(name)
    if fn is None:
        print("krgsh: %s: not a cluster built-in (try help, or: bash \"%s\")" %
              (name, line))
        return 127
    return fn(nodes, rest) or 0


def main(argv):
    nodes = read_nodes()
    if len(argv) > 2 and argv[1] in ("-c", "--command"):
        return run_line(" ".join(argv[2:]), nodes)

    if not sys.stdin.isatty():
        rc = 0
        for line in sys.stdin:
            line = line.strip()
            if line:
                rc = run_line(line, nodes)
        return rc

    host = socket.gethostname()
    while True:
        try:
            line = input("krgsh@%s> " % host)
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        try:
            run_line(line, nodes)
        except SystemExit:
            return 0
        except Exception as exc:                       # noqa: BLE001
            print("krgsh: %s" % exc)


if __name__ == "__main__":
    sys.exit(main(sys.argv))