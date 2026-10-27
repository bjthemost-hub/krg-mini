#!/usr/bin/env python3
"""krg-top - live status board for a krg-mini cluster.

One file, standard library only. Every few seconds it asks each node in the node
list for its /proc counters through the cluster's own `krgtool exec` path, so the
board shows exactly what the cluster can reach. Two /proc/stat samples are kept per
node so the counters can be turned into a CPU percentage, and the page is a single
HTML file the browser refreshes by polling /api/stats.

  krg-top [--port 8099] [--interval 2.0] [--nodes /etc/krg-nodes.list]
  open http://<head-node>:8099/
"""
import argparse
import json
import os
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_NODES = os.environ.get("KRGSH_NODES", "/etc/krg-nodes.list")
KRGTOOL = os.environ.get("KRGTOOL") or (
    "/usr/local/bin/krgtool" if os.path.exists("/usr/local/bin/krgtool")
    else "/root/krg-mini/user/krgtool")

# One line on purpose: `krgtool exec` runs a single command line per request, and a
# multi-line script silently executes only its first line.
PROBE = (
    'echo "@@@ host"; hostname; '
    'echo "@@@ kernel"; uname -r; '
    'echo "@@@ virt"; systemd-detect-virt 2>/dev/null || echo none; '
    'echo "@@@ id"; cat /sys/module/krg_taskview/parameters/node_id 2>/dev/null || echo -; '
    'echo "@@@ cores"; nproc; '
    'echo "@@@ uptime"; cut -d" " -f1 /proc/uptime; '
    'echo "@@@ load"; cat /proc/loadavg; '
    'echo "@@@ stat"; head -1 /proc/stat; '
    'echo "@@@ mem"; grep -E "^(MemTotal|MemFree|MemAvailable):" /proc/meminfo; '
    'echo "@@@ procs"; ps -eo comm=,args= | grep -cE "^python3 .*krg-crack --work[e]r"; '
    'echo "@@@ root"; df -Pk / | tail -1; '
    'echo "@@@ end"'
)


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


def parse_probe(text):
    out = {}
    cur = None
    for line in text.splitlines():
        if line.startswith("@@@ "):
            cur = line[4:].strip()
            if cur == "end":
                cur = None
            continue
        if cur is not None:
            out.setdefault(cur, []).append(line)
    return out


def probe(addr, timeout=4.0):
    """Return the node's raw counters, or None if it did not answer in time."""
    try:
        res = subprocess.run([KRGTOOL, "exec", addr, PROBE],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=timeout)
    except (subprocess.TimeoutExpired, OSError):
        return None
    text = res.stdout.decode("utf-8", "replace")
    if res.returncode != 0 or "@@@ end" not in text:
        return None
    return parse_probe(text)


def cpu_counters(line):
    parts = line.split()
    vals = [int(x) for x in parts[1:]]
    idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
    total = sum(vals[:8]) if len(vals) >= 8 else sum(vals)
    return total, idle


def sample(name, addr, state, lock):
    raw = probe(addr)
    now = time.time()
    if raw is None:
        with lock:
            prev = state.get(name) or {}
            state[name] = {"name": name, "addr": addr, "online": False, "ts": now,
                           "cpu": None, "mem_used": 0, "mem_total": 0, "workers": 0,
                           "_total": prev.get("_total"), "_idle": prev.get("_idle")}
        return

    stat = (raw.get("stat") or [""])[0]
    total, idle = cpu_counters(stat) if stat.startswith("cpu ") else (0, 0)

    mem = {}
    for line in raw.get("mem", []):
        bits = line.split(":")
        if len(bits) == 2:
            mem[bits[0]] = int(bits[1].split()[0])
    mem_total = mem.get("MemTotal", 0)
    mem_avail = mem.get("MemAvailable", mem.get("MemFree", 0))

    load = []
    if raw.get("load"):
        try:
            load = [float(x) for x in raw["load"][0].split()[:3]]
        except ValueError:
            load = []

    disk = None
    if raw.get("root"):
        bits = raw["root"][0].split()
        if len(bits) >= 3 and bits[1].isdigit() and int(bits[1]) > 0:
            disk = round(100.0 * int(bits[2]) / int(bits[1]), 1)

    with lock:
        prev = state.get(name) or {}
        cpu = None
        if prev.get("_total") and total > prev["_total"]:
            cpu = round(100.0 * (1.0 - (idle - prev["_idle"]) / (total - prev["_total"])), 1)
            cpu = max(0.0, min(100.0, cpu))
        state[name] = {
            "name": name, "addr": addr, "online": True, "ts": now,
            "host": (raw.get("host") or [name])[0],
            "kernel": (raw.get("kernel") or ["?"])[0],
            "virt": (raw.get("virt") or ["none"])[0],
            "node_id": (raw.get("id") or ["-"])[0],
            "cores": int((raw.get("cores") or ["0"])[0] or 0),
            "uptime": int(float((raw.get("uptime") or ["0"])[0] or 0)),
            "load": load, "cpu": cpu,
            "mem_total": mem_total,
            "mem_used": max(0, mem_total - mem_avail),
            "workers": int((raw.get("procs") or ["0"])[0] or 0),
            "disk": disk,
            "_total": total, "_idle": idle,
        }


class Board:
    def __init__(self, nodes, interval):
        self.nodes = nodes
        self.interval = interval
        self.state = {}
        self.history = []
        self.lock = threading.Lock()
        self.started = time.time()

    def cycle(self):
        with ThreadPoolExecutor(max_workers=max(4, len(self.nodes))) as pool:
            for name, addr in self.nodes:
                pool.submit(sample, name, addr, self.state, self.lock)

    def loop(self):
        while True:
            t0 = time.time()
            self.cycle()
            with self.lock:
                live = [n for n in self.state.values() if n.get("online")]
                cpu = [n["cpu"] for n in live if n.get("cpu") is not None]
                mem_used = sum(n.get("mem_used", 0) for n in live)
                mem_total = sum(n.get("mem_total", 0) for n in live)
                self.history.append({
                    "t": int(time.time()),
                    "cpu": round(sum(cpu) / len(cpu), 1) if cpu else 0,
                    "mem": round(100.0 * mem_used / mem_total, 1) if mem_total else 0,
                })
                del self.history[:-120]
            time.sleep(max(0.2, self.interval - (time.time() - t0)))

    def snapshot(self):
        with self.lock:
            nodes = sorted(self.state.values(), key=lambda n: n["name"])
            hist = list(self.history)
        live = [n for n in nodes if n.get("online")]
        cpu = [n["cpu"] for n in live if n.get("cpu") is not None]
        return {
            "ts": time.time(), "uptime": int(time.time() - self.started),
            "nodes": nodes, "history": hist,
            "totals": {
                "nodes": len(nodes), "online": len(live),
                "cores": sum(n.get("cores", 0) for n in live),
                "cpu": round(sum(cpu) / len(cpu), 1) if cpu else 0,
                "mem_used": sum(n.get("mem_used", 0) for n in live),
                "mem_total": sum(n.get("mem_total", 0) for n in live),
                "workers": sum(n.get("workers", 0) for n in live),
            },
        }


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>krg-mini cluster</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#e6edf3;--dim:#9aa4af;
      --ok:#3fb950;--warn:#d29922;--bad:#f85149;--acc:#58a6ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
     font:17px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
header{padding:16px 22px;border-bottom:1px solid var(--line);
       display:flex;gap:30px;align-items:baseline;flex-wrap:wrap}
h1{font-size:22px;margin:0;letter-spacing:.5px}
.kv{color:var(--dim);font-size:17px} .kv b{color:var(--fg);font-weight:600}
main{padding:18px 22px}
#grid{display:grid;gap:14px;grid-template-columns:repeat(4,minmax(0,1fr))}
@media (max-width:1500px){#grid{grid-template-columns:repeat(4,minmax(0,1fr))}}
@media (max-width:1180px){#grid{grid-template-columns:repeat(3,minmax(0,1fr))}}
@media (max-width:880px){#grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media (max-width:600px){#grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;
      padding:16px 18px}
.card.off{opacity:.55;border-style:dashed}
.card .top{display:flex;justify-content:space-between;align-items:baseline;gap:10px}
.name{font-weight:700;font-size:21px}
.badge{font-size:14px;padding:2px 8px;border-radius:7px;white-space:nowrap;
       background:#21262d;color:var(--dim);border:1px solid var(--line)}
.badge.phys{color:#79c0ff;border-color:#1f6feb55}
.badge.vm{color:#d2a8ff;border-color:#8957e555}
.row{display:flex;justify-content:space-between;gap:10px;margin-top:12px;
     font-size:16px;color:var(--dim)}
.row b{color:var(--fg);font-weight:600}
.bar{height:9px;background:#21262d;border-radius:5px;overflow:hidden;margin-top:5px}
.bar>i{display:block;height:100%;background:var(--ok);transition:width .4s}
.bar>i.warn{background:var(--warn)} .bar>i.bad{background:var(--bad)}
.sub{margin-top:11px;font-size:14px;color:#7d8590;white-space:nowrap;
     overflow:hidden;text-overflow:ellipsis}
.offline{color:var(--bad);font-size:16px;margin-top:12px}
canvas{width:100%;height:80px;display:block;margin-top:6px}
.legend{font-size:14px;color:var(--dim);display:flex;gap:18px;margin-top:4px}</style></head><body>
<header>
  <h1>krg-mini cluster</h1>
  <div class="kv">nodes <b id="t-nodes">-</b></div>
  <div class="kv">cores <b id="t-cores">-</b></div>
  <div class="kv">cpu <b id="t-cpu">-</b></div>
  <div class="kv">mem <b id="t-mem">-</b></div>
  <div class="kv">workers <b id="t-workers">-</b></div>
  <div class="kv">updated <b id="t-time">-</b></div>
</header>
<main>
  <canvas id="spark"></canvas>
  <div class="legend"><span style="color:#3fb950">cluster cpu %</span>
  <span style="color:#58a6ff">cluster mem %</span></div>
  <div id="grid"></div>
</main>
<script>
const GB = 1024*1024;
function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
  m=Math.floor(s%3600/60);
  return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m';}
function bar(pct){
  const cls = pct>=90?'bad':pct>=70?'warn':'';
  return '<div class="bar"><i class="'+cls+'" style="width:'+
    Math.min(100,pct||0)+'%"></i></div>';
}
function card(n){
  if(!n.online){
    return '<div class="card off"><div class="top"><span class="name">'+n.name+
      '</span><span class="badge">unreachable</span></div>'+
      '<div class="offline">no answer on '+n.addr+'</div></div>';
  }
  const cpu = n.cpu===null?0:n.cpu;
  const memPct = n.mem_total? 100*n.mem_used/n.mem_total : 0;
  const vmap = {oracle:'virtualbox', kvm:'kvm', none:'physical'};
  const virt = vmap[n.virt] || n.virt;
  const cls = (n.virt==='none')?'phys':'vm';
  const load = (n.load||[]).map(function(x){return x.toFixed(2);}).join('  ');
  return '<div class="card">'+
    '<div class="top"><span class="name">'+n.name+'</span>'+
      '<span class="badge '+cls+'">'+virt+' &middot; id '+n.node_id+'</span></div>'+
    '<div class="row"><span>cpu '+n.cores+' cores</span><b>'+
      (n.cpu===null?'-':cpu.toFixed(1)+' %')+'</b></div>'+bar(cpu)+
    '<div class="row"><span>memory</span><b>'+(n.mem_used/GB).toFixed(1)+' / '+
      (n.mem_total/GB).toFixed(1)+' GiB</b></div>'+bar(memPct)+
    '<div class="row"><span>load 1/5/15</span><b>'+(load||'-')+'</b></div>'+
    '<div class="row"><span>krg-crack workers</span><b>'+n.workers+'</b></div>'+
    '<div class="sub">'+n.kernel+' &middot; up '+fmtUp(n.uptime)+
      ' &middot; disk '+(n.disk===null?'-':n.disk+'%')+'</div></div>';
}
function spark(hist){
  const c=document.getElementById('spark'), ctx=c.getContext('2d');
  const dpr = window.devicePixelRatio||1;
  const w=c.width=c.clientWidth*dpr, h=c.height=64*dpr;
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#21262d'; ctx.lineWidth=1;
  for(let i=1;i<4;i++){ctx.beginPath();ctx.moveTo(0,h*i/4);ctx.lineTo(w,h*i/4);ctx.stroke();}
  if(hist.length<2) return;
  function draw(key,color){
    ctx.beginPath();
    hist.forEach(function(p,i){
      const x=w*i/(hist.length-1), y=h-(p[key]/100)*h;
      i?ctx.lineTo(x,y):ctx.moveTo(x,y);
    });
    ctx.strokeStyle=color; ctx.lineWidth=1.6*dpr; ctx.stroke();
  }
  draw('cpu','#3fb950'); draw('mem','#58a6ff');
}
async function tick(){
  try{
    const r = await fetch('/api/stats',{cache:'no-store'});
    const d = await r.json();
    const t = d.totals;
    document.getElementById('t-nodes').textContent = t.online+'/'+t.nodes;
    document.getElementById('t-cores').textContent = t.cores;
    document.getElementById('t-cpu').textContent = t.cpu+' %';
    document.getElementById('t-mem').textContent =
      (t.mem_used/GB).toFixed(1)+' / '+(t.mem_total/GB).toFixed(1)+' GiB';
    document.getElementById('t-workers').textContent = t.workers;
    document.getElementById('t-time').textContent =
      new Date(d.ts*1000).toLocaleTimeString();
    document.getElementById('grid').innerHTML = d.nodes.map(card).join('');
    spark(d.history);
  }catch(e){ /* keep the last frame */ }
}
tick(); setInterval(tick, __INTERVAL_MS__);
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    board = None

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/api/stats"):
            body = json.dumps(self.board.snapshot()).encode()
            ctype = "application/json"
        elif self.path in ("/", "/index.html"):
            body = PAGE.replace("__INTERVAL_MS__",
                                str(int(self.board.interval * 1000))).encode()
            ctype = "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--nodes", default=DEFAULT_NODES)
    args = ap.parse_args()

    nodes = read_nodes(args.nodes)
    if not nodes:
        raise SystemExit("no nodes in %s" % args.nodes)

    board = Board(nodes, args.interval)
    board.cycle()
    threading.Thread(target=board.loop, daemon=True).start()

    Handler.board = board
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print("krg-top on :%d, %d nodes, %.1fs" % (args.port, len(nodes), args.interval),
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()