# 工作报告 2026-10-28  P1 跨节点起进程 / 40 节点铺开

## 一、做了什么

1. 找到进 VM 的通道：`/root/make_fleet.sh`（nd13）里写着 cloud-init 的 `chpasswd`
   是 **root / zz213**，SSH 转发 `192.168.6.13:230<i>`。之前以为密码未知，是没查这份脚本。
2. 给 `krg13-1`/`krg13-2` 加 VirtualBox UDP 端口转发，装 `krg_rpc.ko`，跑通"在 A 上发一条，
   B 上真的跑起来"。
3. 修两处设计问题（见第三节）。
4. 把部署写成两个脚本 + 一张 40 行节点表，铺到 5 台物理机上的 40 台 VM。
5. 起回 nd12 上原先关机的 `krg12-5..8`；nd11 的 ufw 放行 `7901:7910/udp`（仅 192.168.6.0/24）。

## 二、动了哪些文件

新建（本机 `E:\Codex\2026-10-28\krg-p1-remote-exec\`）：

- `scripts\cluster.rpc.list` —— 40 行，`node_id ip vm_index udp_port`，节点号/端口的单一出处
- `scripts\deploy_rpc_vm.sh` —— 在 VM 里执行的装载脚本（rmmod → insmod，带全量 peers）
- `scripts\deploy_host.ps1` —— 从本机驱动：加 NAT 规则、拷文件、装载
- `scripts\verify_rpc.ps1` —— 验收：spawn → 目标机 ps → 回源机 kill → 目标机复查
- `scripts\krg_rpc.ko`、`scripts\krgrpctool` —— 当次编译产物（与 VM 上一致）

修改：

- `scripts\krg_rpc.c` —— 两处，见第三节
- nd13 `/root/krg51/krg/module/krg_rpc.c` —— 与上面同一份拷贝
- nd11 `/etc/ufw` —— 新规则 `7901:7910/udp` 允许 192.168.6.0/24

在 40 台 VM 上（每台）：

- `/root/krg_rpc.ko`、`/root/krgrpctool`、`/root/deploy_rpc_vm.sh`
- 运行时：`krg_rpc` 模块（`self_id=<node>` `rpc_port=7910` `peers=` 39 条）

## 三、两处设计问题（为什么改）

### 3.1 回包地址

原实现回包到请求的 `from`（源地址:源端口），本意是"穿 NAT"。实测：

- VM1 → VM2 请求能到，VM2 `dmesg` 里 `spawned pid=310455`，但 VM1 这边 `tx=4 timeouts=4`。
- 原因：VirtualBox NAT 属于对称型，只接受来自"当初发出去的那个目的端点"的回包；
  `from` 里是 NAT 重写后的主机地址 + 每条流的临时端口，回过去直接被丢。

改成回 **peers 表里对方的转发端口**。这在目标看来是一条新的入向流，NAT 的端口转发会
DNAT 进 guest，立即通。

### 3.2 gpid 是 sh 的 pid

原实现用 `sh -c "<cmd>"`，`ctx.pid` 拿到的是 sh；kill 掉 sh 之后真正的程序变孤儿。
实测第一次跨节点 kill 后，目标机上还剩 1 条 `sleep 300`。

改成 `sh -c "exec <cmd>"`：sh 用 exec 顶替自己，pid 就是程序本身的。
改动后 `ps` 里只有一条 `sleep 300`，kill 即消失。

## 四、验证结果

### 4.1 单对（krg13-1 → krg13-2，2026-10-28 13:16）

```
VM1: ./krgrpctool spawn 2 sleep 300      -> gpid 33866903
VM2: ps -ef                             -> pid 312471 sleep 300
     33866903 = 2<<24 | 312471
VM1: ./krgrpctool kill 33866903         -> killed 33866903
VM2: 0.8 s 后 ps 里 sleep 300 剩 0 条
```

### 4.2 跨物理机四对（`verify_rpc.ps1`，全部 PASS）

| 源节点 | 目标节点 | gpid | 目标机 pid | 结果 |
| --- | --- | --- | --- | --- |
| 1  | 33 | 553965998 | 317870 | PASS |
| 17 | 5  | 84204164  | 318084 | PASS |
| 9  | 24 | 402971344 | 318160 | PASS |
| 25 | 40 | 671406559 | 317919 | PASS |

每对都做了：spawn → 目标机 `ps -p <pid> -o pid=,comm=,args=` 看得到 → 回源机 kill →
0.8 s 后目标机 `ps -p <pid> | wc -l` = 0。

### 4.3 全集群视图

从节点 17 上 `krgtool show 192.168.6.13:7801,192.168.6.21:7801`，能看到 spawn 出来的进程，
gpid 与 `krgrpctool spawn` 返回值一致：

```
192.168.6.21 553966655 318527 2 S 318527 1044 sleep
```
## 五、落点策略（判据③）

### 做了什么

内核里加了两种消息：`KRG_MSG_LOAD`（问"你多闲"）/ `KRG_MSG_LOAD_REPLY`。
`krgrpctool spawn 0 <cmd>` 表示"你自己挑"，模块会：

1. 向 peer 表全部节点各发一条 LOAD；
2. `msleep(300)` 收答案；
3. 按 `score = load(1/256 单位) * 1000000 + 内存缺口` 取最小。
   load 项的量级远大于内存项，所以"有空核"永远优先于"内存多"；
4. 超过 `KRG_LOAD_TTL_MS`(2000 ms) 没回样本的节点直接跳过，不当成空闲。

两个必须记下来的坑：

- **LOAD_REPLY 又踩了同一条**：第一版回到 `from` 源地址，被对称 NAT 丢掉，
  自测 6 次全落在本机（因为别人根本没回）。改成回 peers 表里的端口后正常。
- **平手全压一台**：一开始所有节点 load 都是 0、score 相同，结果全落在节点号最小的那台。
  改成从上一次中选的下一号开始扫，平手就轮转。

### 实测

1. 空载连发 8 次 `spawn 0`：落在 node 1,2,3,6,10,12,13,15 —— 散开了。
2. 压满 node 3（krg11-3，2 vCPU，两个 `yes`，75 s 后 `loadavg` 实测 1.43），
   从 node 17 连发 10 次 `spawn 0`：落在 16,20,21,22,23,26,39,1,2,4，node 3 一次没被选中。
3. 证明"没选中"不是"没回答"：停掉压测、loadavg 降到 0.33 后，
   单独发一次 `spawn 0`，node 3 的 `rx` 从 44 变 45、`tx` 从 44 变 45 ——
   它收到了查询并回了答案，只因 load 高于别人而落选。

### 当前模块版本

40 台全部是 `krg_rpc.ko` md5 `f2a3d2d7754daa9550897456b6052b1a`
（`check_fleet.ps1` 逐台比对 md5 + lsmod，报 40/40 OK）。
## 六、遗留与下一步

1. **落点只在 spawn 上**：`fork` 出来的子进程还没有按这套规则落到别的节点，那是 P1 收尾的下一步。
2. **NODE 列显示 IP 不是主机名**：`krgtool show` 对远端节点显示 `192.168.6.21`，
   原因是工具侧的名称表没覆盖到远端。属显示问题，不影响功能。
3. **两套传输并存**：`krg_rpc`（UDP，内核）与原来的 `krgserve`/`krgtool`（TCP，用户态）
   各走各的。长期应该合成一套，内核 RPC 做主干。
4. **NAT 决定的上限**：每台 VM 占一个外部 UDP 端口才通。要真"一块 CPU"，VM 之间需要能直连
   （bridged / natnetwork / hostonly），这个改法要不要动，等周公定。
5. **部署未做开机自启**：目前是手工 insmod，VM 重启后要重跑 `deploy_host.ps1`。
6. **load 用的是 1 分钟 loadavg，不是 runqueue**：5.1 里 `nr_running()` 没有导出给模块，
   只有 `avenrun[]` 可用。所以刚压上去的头几十秒，判断会偏保守（要等 loadavg 爬起来）。
