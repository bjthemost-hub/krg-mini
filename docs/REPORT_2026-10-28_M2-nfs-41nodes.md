# 工作报告 2026-10-28 — M2「同一份字节」：NFS 共享树跑通（41 + node24）

## 一、做了什么

1. 选服务端：`192.168.6.13`（Ubuntu 24.04，128 GB 内存，5 台宿主之一）。装 `nfs-kernel-server`，
   `mkdir /srv/krg`，导出 `/srv/krg 192.168.6.0/24(rw,sync,no_subtree_check,no_root_squash,insecure)`，
   `exportfs -ra`。**`insecure` 是关键**：VirtualBox NAT 会把客户端源端口改写成高端口，
   不加这一项 NFSv3 直接 `access denied by server`。
2. node 41（VM `123234`，192.168.6.120 的 NAT 里）装 `nfs-common`，`mount -t nfs -o vers=4.0 192.168.6.13:/srv/krg /krg`。
3. node 24（`krg13-8`，192.168.6.13:2308）同样装 `nfs-common` 并挂同一棵。
4. 双向读写 + inode 比对。

## 二、动了哪些文件

服务端 `192.168.6.13`：
- 安装 `nfs-kernel-server`；`/etc/exports` 写入上面那一行；`/srv/krg` 新建。

node 41（VM 123234）：
- 安装 `nfs-common`；`mkdir /krg` 并挂载；`/krg/from41.txt`（测试文件）。

node 24（`krg13-8`）：
- 安装 `nfs-common`；`mkdir /krg` 并挂载；`/krg/from24.txt`（测试文件）。

本机：
- `E:\Codex\2026-10-28\krg-m2-nfs\work\nfs_server_setup.sh`、`nfs_fix.sh`、`m2_test.sh`、`m2_join.sh`
- 本文件

## 三、验证结果（2026-10-28，全部实测）

| 检查 | 命令 | 结果 |
|---|---|---|
| 41 挂载 | `df -h /krg` | `192.168.6.13:/srv/krg 547G 131G 389G 26% /krg` |
| 24 挂载 | `df -h /krg` | 同上 |
| 41 写 | `echo hello-from-41 ... > /krg/from41.txt` | 40 B，inode **21102595** |
| 24 读 41 的文件 | `cat /krg/from41.txt` | `hello-from-41 2026-10-28T15:54:06+00:00` |
| 24 写 | `echo hello-from-krg13-8 ... > /krg/from24.txt` | 45 B，inode **21102596** |
| 41 读 24 的文件 | `cat /krg/from24.txt` | `hello-from-krg13-8 2026-10-28T15:54:54+00:00` |
| inode 一致性 | 41 与 24 上 `stat` | 41 看到 from24 = 21102596；24 看到自己 = 21102596 → **同一 inode** |
| 服务端可见 | 6.13 `cat /srv/krg/from41.txt` | 与 41 写的完全一致 |

## 四、遗留与下一步

1. **只推了 2 台 + 服务端**，40 台全量还没挂；也没写 `/etc/fstab`（重启会掉）。
2. **NFSv4.1 / 4.2 在这颗 5.1 内核上不支持**：`vers=4.1` / `4.2` 都报 `an incorrect mount option was specified`，
   只有 `vers=4.0` 能挂。内核 config 模板里 `CONFIG_NFS_V4_1=y`、`CONFIG_NFS_V4_2=y`，
   实际内核与这份模板不一致 —— 要确认现在跑的 bzImage 是哪份 config 编的。
3. **NFS 服务端在宿主 6.13，不在节点里**：这是 NAT 下的权宜做法（VM 不能直连）。
   真要节点间共享，得先解决 VM 之间直连（bridged / natnetwork），这条等周公定。
4. 目录权限：`/srv/krg` 是 root:root 755，只有 root 能写；后面给 GCC 用时得按用途开子目录。
5. 没有测"两个节点同时写同一个文件"的语义（那是 M3 的范围，不是 M2）。

## 五、补：全量推完（2026-10-28 16:00 左右）

40 台全部按宿主分批挂上 `/krg`，`/etc/fstab` 也写了（`vers=4.0,hard,intr,noatime`），逐台输出
`OK <host> 0 inode41=21102595`：

| 宿主 | 节点 | 结果 |
|---|---|---|
| 192.168.6.13 | 17–24 | 8/8 OK |
| 192.168.6.11 | 1–8 | 8/8 OK |
| 192.168.6.12 | 9–16 | 8/8 OK |
| 192.168.6.20 | 25–32 | 8/8 OK |
| 192.168.6.21 | 33–40 | 8/8 OK |

加 node 41，共 **41 个节点**看到同一份文件（`from41.txt` inode 21102595，四十台报的 inode 全一样）。

### 实战验收：写、编、跑分在三个节点

| 步骤 | 在哪 | 命令 | 结果 |
|---|---|---|---|
| 写源码 | node 3（`krg11-3`） | `cp hello.c /krg/m2/hello.c` | 261 B，md5 `224541e007d401b43fe1b65e4e9e1059` |
| 读到同一份 | node 40（`krg21-8`） | `md5sum /krg/m2/hello.c` | 同一个 md5 |
| 编译 | node 40 | `gcc -O2 /krg/m2/hello.c -o /krg/m2/hello` | 16064 B，ELF x86-64 PIE，md5 `8f63f6ff28ab124d24eba072d385a375` |
| 立刻看到产物并执行 | node 3 | `/krg/m2/hello` | `hello from krg11-3, sum=499999500000` |

这一条同时说明两件事：① 不同节点上确实是同一份字节（M2 判据）；
② **GCC 能在 5.1 内核的节点上正常编译并产出可执行文件**（单机范围内，跨节点执行还没做）。

### 新增文件

- `E:\Codex\2026-10-28\krg-m2-nfs\work\join_krg.sh`（每台节点执行的挂载脚本）
- `E:\Codex\2026-10-28\krg-m2-nfs\work\deploy_nfs.ps1`（按宿主批量推）
- `E:\Codex\2026-10-28\krg-m2-nfs\work\hello.c`

### 这条之后仍未做的

- `/krg` 的写一致性不在 M2 范围（多节点同时写同一文件没测）。
- 跨节点编译（cpp 在 A、cc1 在 B）要等 M1 的进程树，今天只是"同一台编译、另一台立刻看到"。
