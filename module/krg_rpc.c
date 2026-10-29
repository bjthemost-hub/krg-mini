// SPDX-License-Identifier: GPL-2.0
/*
 * krg_rpc - khz kernel-to-kernel request/reply channel (P1 of the SSI roadmap).
 *
 * Goal: one node's *kernel* creates a task on another node's *kernel* and gets
 * back its cluster-wide pid (gpid), with no userspace helper in the data path.
 *
 *   transport   UDP, one datagram per request / per reply
 *   idempotency every request carries a 64-bit id; the receiver caches
 *               (id -> gpid) and answers a repeat with the same gpid instead of
 *               spawning a second task
 *   timeout     2 s per attempt, one retry with the *same* id; on final timeout
 *               the caller gets -ETIMEDOUT and the remote task is left running
 *   interface   /dev/krgrpc, ioctl only
 *
 * Module params:
 *   self_id=13                  this node's id (0..255), goes into the gpid
 *   port=7910                   UDP port both sides listen on
 *   peers="11:192.168.6.11,12:192.168.6.12"
 *                               node id -> address table
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/kmod.h>
#include <linux/sched.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <net/sock.h>

#define KRG_MAGIC	0x4b524750u	/* "KRGP" */
#define KRG_CMD_MAX	192
#define KRG_MAX_NODE	256
#define KRG_SLOTS	128

#define KRG_WAIT_MS	2000
#define KRG_TRIES	2

enum krg_msg_type {
	KRG_MSG_SPAWN = 1,
	KRG_MSG_KILL  = 2,
	KRG_MSG_REPLY = 3,
	KRG_MSG_LOAD  = 4,	/* "how busy are you?" -- asks for a load sample */
	KRG_MSG_LOAD_REPLY = 5,
};

struct krg_msg {
	__u32 magic;
	__u16 type;
	__u16 reserved;
	__u64 reqid;
	__u32 src_node;
	__u32 dst_node;
	__s32 status;
	__u32 gpid;
	char  cmd[KRG_CMD_MAX];
};

/* ---- ioctl ABI ---------------------------------------------------------- */

struct krg_rpc_spawn {
	__u32 node;
	__u32 gpid;
	__s32 status;
	__u32 pad;
	char  cmd[KRG_CMD_MAX];
};

struct krg_rpc_kill {
	__u32 gpid;
	__s32 status;
	__u32 pad[2];
};

struct krg_rpc_info {
	__u32 self;
	__u32 port;
	__u32 npeers;
	__u32 rx_seen;
	__u32 tx_sent;
	__u32 dedup_hit;
	__u32 timeouts;
	__u32 rx_errors;
};

#define KRG_IOC_SPAWN	_IOWR('R', 10, struct krg_rpc_spawn)
#define KRG_IOC_KILL	_IOWR('R', 11, struct krg_rpc_kill)
#define KRG_IOC_INFO	_IOR('R', 12, struct krg_rpc_info)

/* ---- tunables ----------------------------------------------------------- */

static unsigned int self_id;
module_param(self_id, uint, 0444);
MODULE_PARM_DESC(self_id, "cluster node id (0..255), used in gpid");

static unsigned int rpc_port = 7910;
module_param(rpc_port, uint, 0444);
MODULE_PARM_DESC(rpc_port, "UDP port for kernel-to-kernel messages");

static char *peers;
module_param(peers, charp, 0444);
MODULE_PARM_DESC(peers, "node id -> address[:port] table, \"11:192.168.6.11,12:192.168.6.13:7912\"");

/* ---- state -------------------------------------------------------------- */

struct krg_peer {
	__be32 addr;
	__be16 port;
	u8     valid;
};

static struct krg_peer peer_tab[KRG_MAX_NODE];
static u32 peer_count;
static struct socket *rx_sock;
static struct task_struct *rx_task;
static atomic64_t reqid_seq = ATOMIC64_INIT(1);

/*
 * Load samples, one slot per node.  Every node answers a KRG_MSG_LOAD with its
 * run-queue length and free memory; the caller samples all peers and keeps the
 * freshest answer.  Samples older than KRG_LOAD_TTL_MS are ignored, so a dead
 * node simply never wins the placement instead of hanging the call.
 */
#define KRG_LOAD_TTL_MS	2000

/*
 * One node's answer to "how busy are you?".  `load` is the 1-minute load
 * average in 1/256 units (q8) so the fractional part survives the trip through
 * an integer message field.
 */
struct krg_sample {
	unsigned long stamp;	/* jiffies when the sample arrived */
	u32 load;		/* loadavg * 256 */
	u32 freemb;
	u8  valid;
};

static struct krg_sample load_tab[KRG_MAX_NODE];
static DEFINE_SPINLOCK(load_lock);
static u32 last_pick;
static atomic_t stat_rx = ATOMIC_INIT(0);
static atomic_t stat_tx = ATOMIC_INIT(0);
static atomic_t stat_dedup = ATOMIC_INIT(0);
static atomic_t stat_timeout = ATOMIC_INIT(0);
static atomic_t stat_rxerr = ATOMIC_INIT(0);

struct krg_pend {
	u64 reqid;
	s32 status;
	u32 gpid;
	int ref;
	struct completion done;
};

static struct krg_pend *pend_tab[KRG_SLOTS];
static DEFINE_SPINLOCK(pend_lock);

struct krg_dedup {
	u64 reqid;
	u32 gpid;
	unsigned long exp;
};

static struct krg_dedup dedup_tab[KRG_SLOTS];
static DEFINE_SPINLOCK(dedup_lock);

/* ---- helpers ------------------------------------------------------------ */

static inline u32 krg_gpid_of(pid_t pid)
{
	return ((u32)(self_id & 0xff) << 24) | ((u32)pid & 0x00ffffffu);
}

static u64 krg_new_reqid(void)
{
	return (u64)atomic64_inc_return(&reqid_seq);
}

static int krg_peer_addr(u32 node, __be32 *out, __be16 *port)
{
	if (node >= KRG_MAX_NODE || !peer_tab[node].valid)
		return -EHOSTUNREACH;
	*out = peer_tab[node].addr;
	*port = peer_tab[node].port;
	return 0;
}

static int krg_send(const struct krg_msg *m, __be32 addr, __be16 port)
{
	struct sockaddr_in sa;
	struct socket *sock;
	struct kvec iov;
	struct msghdr msg;
	int ret;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = port;
	sa.sin_addr.s_addr = addr;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);

	iov.iov_base = (void *)m;
	iov.iov_len = sizeof(*m);

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sock);
	if (ret)
		return ret;

	ret = kernel_sendmsg(sock, &msg, &iov, 1, sizeof(*m));
	sock_release(sock);
	if (ret < 0)
		return ret;

	atomic_inc(&stat_tx);
	return 0;
}

static void krg_send_reply(const struct krg_msg *req, struct sockaddr_in *from,
			   s32 status, u32 gpid)
{
	struct krg_msg r;

	memset(&r, 0, sizeof(r));
	r.magic = KRG_MAGIC;
	r.type = KRG_MSG_REPLY;
	r.reqid = req->reqid;
	r.src_node = self_id & 0xff;
	r.dst_node = req->src_node;
	r.status = status;
	r.gpid = gpid;
	/* Reply to the endpoint the peer table holds for the requester.
	 *
	 * Why not reply to `from`: behind VBox NAT the request arrives with the
	 * source rewritten to the host's address and a per-flow external port.
	 * A datagram sent back to that port is dropped, because the NAT engine
	 * only accepts packets coming from the exact remote endpoint the flow
	 * was opened to (symmetric NAT).  Sending to the requester's own
	 * forwarded port instead looks like a fresh inbound flow, which the NAT
	 * port-forward does deliver to the guest.
	 */
	{
		__be32 addr;
		__be16 port;

		if (krg_peer_addr(req->src_node, &addr, &port) == 0)
			krg_send(&r, addr, port);
		else
			krg_send(&r, from->sin_addr.s_addr, from->sin_port);
	}
}

/* ---- local spawn -------------------------------------------------------- */

struct krg_spawn_ctx {
	pid_t pid;
};

/*
 * Runs inside the helper process just before execve, so task_pid_nr(current)
 * is the pid the user program will actually get.  That is how we learn the pid
 * without asking userspace to report it back.
 */
static int krg_umh_init(struct subprocess_info *si, struct cred *new)
{
	struct krg_spawn_ctx *ctx = si->data;

	ctx->pid = task_pid_nr(current);
	return 0;
}

static int krg_local_spawn(const char *cmd, pid_t *out_pid)
{
	static char *envp[] = {
		"HOME=/",
		"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		NULL
	};
	char *argv[4];
	char *shcmd;
	struct subprocess_info *sub;
	struct krg_spawn_ctx ctx;
	int ret;

	if (!cmd || !cmd[0])
		return -EINVAL;

	/* Run the command through `sh -c "exec <cmd>"`.
	 *
	 * The exec matters: plain `sh -c` leaves the shell as the parent, so the
	 * pid we hand back is the shell's, and killing it orphans the real
	 * program.  With `exec` the shell replaces itself, so the pid we report
	 * really is the program's -- kill is then exact, and `ps` shows what the
	 * user asked for instead of a wrapper.
	 */
	shcmd = kasprintf(GFP_KERNEL, "exec %s", cmd);
	if (!shcmd)
		return -ENOMEM;

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = shcmd;
	argv[3] = NULL;

	memset(&ctx, 0, sizeof(ctx));
	sub = call_usermodehelper_setup("/bin/sh", argv, envp, GFP_KERNEL,
					krg_umh_init, NULL, &ctx);
	if (!sub) {
		kfree(shcmd);
		return -ENOMEM;
	}

	ret = call_usermodehelper_exec(sub, UMH_WAIT_EXEC);
	kfree(shcmd);
	if (ret)
		return ret;

	if (ctx.pid <= 0)
		return -EAGAIN;

	*out_pid = ctx.pid;
	return 0;
}

static s32 krg_local_kill(u32 gpid)
{
	u32 nid = (gpid >> 24) & 0xff;
	u32 pid = gpid & 0x00ffffffu;
	struct task_struct *p;
	int ret = -ESRCH;

	if (nid != (self_id & 0xff))
		return -EXDEV;

	rcu_read_lock();
	p = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (p) {
		get_task_struct(p);
		rcu_read_unlock();
		ret = send_sig(SIGKILL, p, 0);
		put_task_struct(p);
		return ret;
	}
	rcu_read_unlock();
	return ret;
}

/* ---- receiver-side dedup ------------------------------------------------ */

static struct krg_dedup *krg_dedup_find_locked(u64 reqid)
{
	int i;

	for (i = 0; i < KRG_SLOTS; i++)
		if (dedup_tab[i].reqid == reqid)
			return &dedup_tab[i];
	return NULL;
}

static void krg_dedup_insert(u64 reqid, u32 gpid)
{
	unsigned long now = jiffies;
	unsigned long flags;
	struct krg_dedup *slot = NULL;
	int i;

	spin_lock_irqsave(&dedup_lock, flags);
	for (i = 0; i < KRG_SLOTS; i++) {
		if (!dedup_tab[i].reqid) {
			slot = &dedup_tab[i];
			break;
		}
	}
	if (!slot) {
		slot = &dedup_tab[0];
		for (i = 1; i < KRG_SLOTS; i++)
			if (time_before(dedup_tab[i].exp, slot->exp))
				slot = &dedup_tab[i];
	}
	slot->reqid = reqid;
	slot->gpid = gpid;
	slot->exp = now + msecs_to_jiffies(60000);
	spin_unlock_irqrestore(&dedup_lock, flags);
}

/* ---- pending request table ---------------------------------------------- */

static struct krg_pend *krg_pend_find_locked(u64 reqid)
{
	int i;

	for (i = 0; i < KRG_SLOTS; i++)
		if (pend_tab[i] && pend_tab[i]->reqid == reqid)
			return pend_tab[i];
	return NULL;
}

/* caller holds pend_lock */
static void krg_pend_put_locked(struct krg_pend *p)
{
	if (--p->ref > 0)
		return;
	kfree(p);
}

static struct krg_pend *krg_pend_new(u64 reqid)
{
	unsigned long flags;
	struct krg_pend *p;
	int i;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return NULL;
	p->reqid = reqid;
	p->ref = 2;
	init_completion(&p->done);

	spin_lock_irqsave(&pend_lock, flags);
	for (i = 0; i < KRG_SLOTS; i++) {
		if (!pend_tab[i]) {
			pend_tab[i] = p;
			spin_unlock_irqrestore(&pend_lock, flags);
			return p;
		}
	}
	spin_unlock_irqrestore(&pend_lock, flags);

	kfree(p);
	return NULL;
}

static void krg_pend_del(struct krg_pend *p)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&pend_lock, flags);
	for (i = 0; i < KRG_SLOTS; i++) {
		if (pend_tab[i] == p) {
			pend_tab[i] = NULL;
			krg_pend_put_locked(p);
			break;
		}
	}
	spin_unlock_irqrestore(&pend_lock, flags);
}

static void krg_pend_put(struct krg_pend *p)
{
	unsigned long flags;

	spin_lock_irqsave(&pend_lock, flags);
	krg_pend_put_locked(p);
	spin_unlock_irqrestore(&pend_lock, flags);
}

static void krg_pend_complete(u64 reqid, s32 status, u32 gpid)
{
	unsigned long flags;
	struct krg_pend *p;

	spin_lock_irqsave(&pend_lock, flags);
	p = krg_pend_find_locked(reqid);
	if (p) {
		p->status = status;
		p->gpid = gpid;
		p->ref++;
	}
	spin_unlock_irqrestore(&pend_lock, flags);

	if (!p)
		return;

	complete(&p->done);
	krg_pend_put(p);
}

/* ---- message handling --------------------------------------------------- */

static s32 krg_handle_spawn(u64 reqid, char *cmd, u32 *out_gpid)
{
	unsigned long flags;
	struct krg_dedup *d;
	pid_t pid = 0;
	u32 gpid;
	s32 ret;

	if (!cmd[0])
		return -EINVAL;

	spin_lock_irqsave(&dedup_lock, flags);
	d = krg_dedup_find_locked(reqid);
	if (d) {
		gpid = d->gpid;
		spin_unlock_irqrestore(&dedup_lock, flags);
		atomic_inc(&stat_dedup);
		*out_gpid = gpid;
		return 0;
	}
	spin_unlock_irqrestore(&dedup_lock, flags);

	ret = krg_local_spawn(cmd, &pid);
	if (ret)
		return ret;

	gpid = krg_gpid_of(pid);
	krg_dedup_insert(reqid, gpid);
	*out_gpid = gpid;
	pr_info("krg_rpc: spawned pid=%d gpid=%u cmd=%.64s\n", pid, gpid, cmd);
	return 0;
}

/* ---- load sampling / placement ------------------------------------------ */

static u32 krg_free_mb(void)
{
	struct sysinfo si;

	si_meminfo(&si);
	return (u32)(((u64)si.freeram * PAGE_SIZE) >> 20);
}

/*
 * avenrun[] is the only load figure exported to modules in 5.1 (nr_running()
 * is not).  It is in 1/2048 units, so >>3 turns it into q8.
 */
static u32 krg_loadavg_q8(void)
{
	return (u32)(avenrun[0] >> 3);
}

static void krg_sample_store(u32 node, u32 load, u32 freemb)
{
	unsigned long flags;

	if (node >= KRG_MAX_NODE)
		return;

	spin_lock_irqsave(&load_lock, flags);
	load_tab[node].load = load;
	load_tab[node].freemb = freemb;
	load_tab[node].stamp = jiffies;
	load_tab[node].valid = 1;
	spin_unlock_irqrestore(&load_lock, flags);
}

static void krg_load_broadcast(void)
{
	struct krg_msg m;
	u32 i;

	memset(&m, 0, sizeof(m));
	m.magic = KRG_MAGIC;
	m.type = KRG_MSG_LOAD;
	m.src_node = self_id & 0xff;
	m.dst_node = 0xff;

	for (i = 0; i < KRG_MAX_NODE; i++) {
		__be32 addr;
		__be16 port;

		if (i == (self_id & 0xff))
			continue;
		if (krg_peer_addr(i, &addr, &port))
			continue;
		m.dst_node = i;
		krg_send(&m, addr, port);
	}
}

/*
 * Pick the least loaded node.  The run-queue length dominates; free memory is
 * only a tie-break, so a node with an idle core always beats a saturated one
 * no matter how much RAM it has.  Nodes that did not answer within the TTL are
 * skipped rather than treated as "free".
 */
static u32 krg_pick_node(void)
{
	unsigned long flags;
	unsigned long now;
	u32 best = self_id & 0xff;
	u32 best_load = 0, best_free = 0;
	u64 best_score = ~0ULL;
	u32 i, idx, start;

	krg_load_broadcast();

	/* one LAN round trip plus slack; a peer that is down must not stall the
	 * caller longer than this */
	msleep(300);

	krg_sample_store(self_id & 0xff, krg_loadavg_q8(), krg_free_mb());
	now = jiffies;

	/* Scan from just after the previous winner.  Idle nodes all score the
	 * same, so without this every request would land on the lowest-numbered
	 * one and the rest of the cluster would sit at zero. */
	start = (last_pick + 1) % KRG_MAX_NODE;

	spin_lock_irqsave(&load_lock, flags);
	for (i = 0; i < KRG_MAX_NODE; i++) {
		struct krg_sample *s;
		u32 mem_term;
		u64 score;

		idx = (start + i) % KRG_MAX_NODE;
		s = &load_tab[idx];
		if (!s->valid)
			continue;
		if (time_after(now,
			       s->stamp + msecs_to_jiffies(KRG_LOAD_TTL_MS)))
			continue;
		mem_term = s->freemb > 1024 ? 0 : (1024 - s->freemb);
		score = (u64)s->load * 1000000ULL + mem_term;
		if (score < best_score) {
			best_score = score;
			best = idx;
			best_load = s->load;
			best_free = s->freemb;
		}
	}
	spin_unlock_irqrestore(&load_lock, flags);

	last_pick = best;
	pr_info("krg_rpc: placement -> node=%u load=%u/256 free=%uMB\n",
		best, best_load, best_free);
	return best;
}

static void krg_handle(struct krg_msg *m, struct sockaddr_in *from)
{
	atomic_inc(&stat_rx);

	if (m->magic != KRG_MAGIC)
		return;
	if (m->dst_node != (self_id & 0xff) && m->dst_node != 0xffu)
		return;

	switch (m->type) {
	case KRG_MSG_SPAWN: {
		u32 gpid = 0;
		s32 st;

		m->cmd[KRG_CMD_MAX - 1] = 0;
		st = krg_handle_spawn(m->reqid, m->cmd, &gpid);
		krg_send_reply(m, from, st, st ? 0 : gpid);
		break;
	}
	case KRG_MSG_KILL: {
		s32 st = krg_local_kill(m->gpid);

		krg_send_reply(m, from, st, m->gpid);
		break;
	}
	case KRG_MSG_REPLY:
		krg_pend_complete(m->reqid, m->status, m->gpid);
		break;
	case KRG_MSG_LOAD: {
		struct krg_msg r;

		memset(&r, 0, sizeof(r));
		r.magic = KRG_MAGIC;
		r.type = KRG_MSG_LOAD_REPLY;
		r.reqid = m->reqid;
		r.src_node = self_id & 0xff;
		r.dst_node = m->src_node;
		r.status = (s32)krg_loadavg_q8();
		r.gpid = krg_free_mb();
		/* same reason as krg_send_reply(): behind NAT the source
		 * address is the host's, so use the peer's own endpoint */
		{
			__be32 addr;
			__be16 port;

			if (krg_peer_addr(m->src_node, &addr, &port) == 0)
				krg_send(&r, addr, port);
			else
				krg_send(&r, from->sin_addr.s_addr, from->sin_port);
		}
		break;
	}
	case KRG_MSG_LOAD_REPLY:
		krg_sample_store(m->src_node, (u32)m->status, m->gpid);
		break;
	default:
		break;
	}
}

static int krg_rx_loop(void *arg)
{
	struct krg_msg *m;
	int err_count = 0;

	m = kmalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		struct sockaddr_in from;
		struct msghdr msg;
		struct kvec iov;
		int len;

		memset(&from, 0, sizeof(from));
		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &from;
		msg.msg_namelen = sizeof(from);

		iov.iov_base = m;
		iov.iov_len = sizeof(*m);

		len = kernel_recvmsg(rx_sock, &msg, &iov, 1, sizeof(*m), 0);
		if (len == -EAGAIN || len == -ERESTARTSYS || len == -ETIMEDOUT)
			continue;
		if (len < 0) {
			if (kthread_should_stop())
				break;
			atomic_inc(&stat_rxerr);
			if (++err_count > 100) {
				pr_warn("krg_rpc: recv keeps failing (%d), back off\n", len);
				msleep(500);
				err_count = 0;
			}
			continue;
		}
		err_count = 0;
		if (len < (int)sizeof(*m))
			continue;
		krg_handle(m, &from);
	}

	kfree(m);
	return 0;
}

/* ---- client side -------------------------------------------------------- */

static int krg_remote_spawn(u32 node, const char *cmd, u32 *out_gpid)
{
	struct krg_msg m;
	struct krg_pend *p;
	__be32 addr;
	__be16 port;
	s64 left;
	int ret, try;

	if (!cmd || !cmd[0])
		return -EINVAL;
	if (strlen(cmd) >= KRG_CMD_MAX)
		return -E2BIG;

	/* node 0 means "you pick": sample the cluster and use the least
	 * loaded node.  This is the seed of the global scheduler -- placement
	 * still runs in the caller's kernel, but the decision is made on data
	 * gathered from every node. */
	if (node == 0)
		node = krg_pick_node();

	if (node == (self_id & 0xff)) {
		pid_t pid = 0;

		ret = krg_local_spawn(cmd, &pid);
		if (ret)
			return ret;
		*out_gpid = krg_gpid_of(pid);
		return 0;
	}

	ret = krg_peer_addr(node, &addr, &port);
	if (ret)
		return ret;

	p = krg_pend_new(krg_new_reqid());
	if (!p)
		return -ENOMEM;

	memset(&m, 0, sizeof(m));
	m.magic = KRG_MAGIC;
	m.type = KRG_MSG_SPAWN;
	m.reqid = p->reqid;
	m.src_node = self_id & 0xff;
	m.dst_node = node;
	strscpy(m.cmd, cmd, sizeof(m.cmd));

	ret = -ETIMEDOUT;
	for (try = 0; try < KRG_TRIES; try++) {
		if (try) {
			reinit_completion(&p->done);
			pr_info("krg_rpc: retry spawn reqid=%llu node=%u\n",
				(unsigned long long)p->reqid, node);
		}
		if (krg_send(&m, addr, port))
			continue;

		left = wait_for_completion_timeout(&p->done,
						   msecs_to_jiffies(KRG_WAIT_MS));
		if (left) {
			ret = p->status;
			if (!ret)
				*out_gpid = p->gpid;
			break;
		}
		atomic_inc(&stat_timeout);
	}

	krg_pend_del(p);
	krg_pend_put(p);
	return ret;
}

static int krg_remote_kill(u32 gpid)
{
	struct krg_msg m;
	struct krg_pend *p;
	__be32 addr;
	__be16 port;
	s64 left;
	u32 node = (gpid >> 24) & 0xff;
	int ret, try;

	if (node == (self_id & 0xff))
		return krg_local_kill(gpid);

	ret = krg_peer_addr(node, &addr, &port);
	if (ret)
		return ret;

	p = krg_pend_new(krg_new_reqid());
	if (!p)
		return -ENOMEM;

	memset(&m, 0, sizeof(m));
	m.magic = KRG_MAGIC;
	m.type = KRG_MSG_KILL;
	m.reqid = p->reqid;
	m.src_node = self_id & 0xff;
	m.dst_node = node;
	m.gpid = gpid;

	ret = -ETIMEDOUT;
	for (try = 0; try < KRG_TRIES; try++) {
		if (try) {
			reinit_completion(&p->done);
			pr_info("krg_rpc: retry kill reqid=%llu node=%u\n",
				(unsigned long long)p->reqid, node);
		}
		if (krg_send(&m, addr, port))
			continue;

		left = wait_for_completion_timeout(&p->done,
						   msecs_to_jiffies(KRG_WAIT_MS));
		if (left) {
			ret = p->status;
			break;
		}
		atomic_inc(&stat_timeout);
	}

	krg_pend_del(p);
	krg_pend_put(p);
	return ret;
}

/* ---- device ------------------------------------------------------------- */

static long krgrpc_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case KRG_IOC_SPAWN: {
		struct krg_rpc_spawn s;
		u32 gpid = 0;
		int ret;

		if (copy_from_user(&s, (void __user *)arg, sizeof(s)))
			return -EFAULT;
		s.cmd[KRG_CMD_MAX - 1] = 0;
		ret = krg_remote_spawn(s.node, s.cmd, &gpid);
		s.status = ret;
		s.gpid = ret ? 0 : gpid;
		if (copy_to_user((void __user *)arg, &s, sizeof(s)))
			return -EFAULT;
		return 0;
	}
	case KRG_IOC_KILL: {
		struct krg_rpc_kill k;
		int ret;

		if (copy_from_user(&k, (void __user *)arg, sizeof(k)))
			return -EFAULT;
		ret = krg_remote_kill(k.gpid);
		k.status = ret;
		if (copy_to_user((void __user *)arg, &k, sizeof(k)))
			return -EFAULT;
		return 0;
	}
	case KRG_IOC_INFO: {
		struct krg_rpc_info i;

		memset(&i, 0, sizeof(i));
		i.self = self_id & 0xff;
		i.port = rpc_port;
		i.npeers = peer_count;
		i.rx_seen = (u32)atomic_read(&stat_rx);
		i.tx_sent = (u32)atomic_read(&stat_tx);
		i.dedup_hit = (u32)atomic_read(&stat_dedup);
		i.timeouts = (u32)atomic_read(&stat_timeout);
		i.rx_errors = (u32)atomic_read(&stat_rxerr);
		if (copy_to_user((void __user *)arg, &i, sizeof(i)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations krgrpc_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = krgrpc_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice krgrpc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "krgrpc",
	.fops = &krgrpc_fops,
	.mode = 0400,
};

/* ---- init / exit -------------------------------------------------------- */

static int krg_parse_peers(void)
{
	char *buf, *p, *tok;

	if (!peers || !peers[0])
		return 0;

	buf = kstrdup(peers, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	p = buf;
	while ((tok = strsep(&p, ",")) != NULL) {
		char *colon, *colon2;
		unsigned int nid = 0, pport = rpc_port;

		if (!*tok)
			continue;
		colon = strchr(tok, ':');
		if (!colon)
			continue;
		*colon = 0;
		if (kstrtouint(tok, 10, &nid))
			continue;
		if (nid >= KRG_MAX_NODE || nid == (self_id & 0xff))
			continue;

		colon2 = strchr(colon + 1, ':');
		if (colon2) {
			*colon2 = 0;
			if (kstrtouint(colon2 + 1, 10, &pport) || pport > 65535)
				continue;
		}

		peer_tab[nid].addr = in_aton(colon + 1);
		peer_tab[nid].port = htons((u16)pport);
		peer_tab[nid].valid = 1;
		peer_count++;
	}

	kfree(buf);
	return 0;
}

static int krg_rx_start(void)
{
	struct sockaddr_in sa;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP,
			       &rx_sock);
	if (ret) {
		pr_err("krg_rpc: socket create failed: %d\n", ret);
		return ret;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((u16)rpc_port);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);

	ret = kernel_bind(rx_sock, (struct sockaddr *)&sa, sizeof(sa));
	if (ret) {
		pr_err("krg_rpc: bind %u failed: %d\n", rpc_port, ret);
		sock_release(rx_sock);
		rx_sock = NULL;
		return ret;
	}

	/* wake the loop once a second so kthread_stop() can take effect */
	rx_sock->sk->sk_rcvtimeo = HZ;

	rx_task = kthread_run(krg_rx_loop, NULL, "krg_rpc_rx");
	if (IS_ERR(rx_task)) {
		ret = PTR_ERR(rx_task);
		rx_task = NULL;
		sock_release(rx_sock);
		rx_sock = NULL;
		return ret;
	}
	return 0;
}

static void krg_rx_stop(void)
{
	if (rx_task) {
		kthread_stop(rx_task);
		rx_task = NULL;
	}
	if (rx_sock) {
		sock_release(rx_sock);
		rx_sock = NULL;
	}
}

static int __init krg_rpc_init(void)
{
	int ret;

	atomic64_set(&reqid_seq, (u64)ktime_get_ns());

	ret = krg_parse_peers();
	if (ret)
		return ret;

	ret = krg_rx_start();
	if (ret)
		return ret;

	ret = misc_register(&krgrpc_dev);
	if (ret) {
		krg_rx_stop();
		return ret;
	}

	pr_info("krg_rpc: up, self_id=%u port=%u peers=%u\n",
		self_id & 0xff, rpc_port, peer_count);
	return 0;
}

static void __exit krg_rpc_exit(void)
{
	misc_deregister(&krgrpc_dev);
	krg_rx_stop();
	pr_info("krg_rpc: down\n");
}

module_init(krg_rpc_init);
module_exit(krg_rpc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("khz: kernel-to-kernel remote spawn / kill (P1)");
