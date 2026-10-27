// SPDX-License-Identifier: GPL-2.0
/* krg_taskview - minimal "single system image" building block.
 *
 * Kernel side:
 *   /dev/krgview  read  : this node's task table, each row carries a
 *                         cluster-wide gpid = (node_id << 24) | pid
 *                 write : "SPAWN <shell command>"  create a task on this node
 *                         "KILL <gpid>"            kill a task by global pid
 *                         "LOAD <node> <runq> <seq>"  feed one node's load
 *                 ioctl : PLACE    pick the idlest node and reserve it
 *                         RELEASE  give a reservation back
 *                         STATE    dump the load table the kernel is using
 *
 * The placement decision (which node, and the reservation that stops a burst
 * of callers from all picking the same idle node) is made here, under one
 * lock. Userspace only collects the per-node numbers and forwards the task to
 * the node this kernel chose.
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/umh.h>
#include <linux/pid.h>
#include <linux/signal.h>

#define KRGV_MAGIC	0x4b524779u	/* "KRGy" - v4: gpid + run + memory */
#define KRGV_MAX_TASKS	4096
#define KRGV_CMD_MAX	512

struct krgv_task {
	__u32 pid;
	__u32 tgid;
	__u32 ppid;
	__u32 state;
	__u32 rss_kb;
	__u32 gpid;
	char  comm[16];
};

struct krgv_hdr {
	__u32 magic;
	__u32 count;
	__u32 max;
	__u32 node_id;
	__u32 run;		/* tasks in TASK_RUNNING (running or runnable) */
	__u32 pad;
	__u32 mem_total_kb;
	__u32 mem_free_kb;
};

/* ---- placement ---------------------------------------------------------- */

#define KRGV_NODES	256	/* node_id is one byte */
#define KRGV_TICKETS	128	/* outstanding reservations we can track */
#define KRGV_TTL_MS	15000	/* a reservation nobody releases expires */

struct krgv_node_load {
	__u32 runq;
	__u32 seq;
	__u32 reserved;
	__u32 valid;
};

struct krgv_place {
	__u32 node;	/* out: chosen node id */
	__u32 ticket;	/* out: (node << 24) | serial; pass to RELEASE */
	__u32 runq;	/* out: that node's runq when the decision was made */
	__u32 self;	/* out: this node's id */
};

struct krgv_state {
	__u32 self;
	__u32 nvalid;
	__u32 tickets_used;
	__u32 pad;
	struct krgv_node_load n[KRGV_NODES];
};

#define KRGV_IOC_MAGIC		'K'
#define KRGV_IOC_PLACE		_IOR(KRGV_IOC_MAGIC, 1, struct krgv_place)
#define KRGV_IOC_RELEASE	_IOW(KRGV_IOC_MAGIC, 2, __u32)
#define KRGV_IOC_STATE		_IOR(KRGV_IOC_MAGIC, 3, struct krgv_state)

static unsigned int node_id = 0;
module_param(node_id, uint, 0444);
MODULE_PARM_DESC(node_id, "cluster node id (1..255), used in gpid");

static inline u32 krgv_gpid(u32 pid)
{
	return ((u32)(node_id & 0xff) << 24) | (pid & 0x00ffffffu);
}

static u32 krgv_state(struct task_struct *p)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	return (u32)READ_ONCE(p->__state);
#else
	return (u32)READ_ONCE(p->state);
#endif
}

/* ---- placement state ---------------------------------------------------- */

static struct krgv_node_load krgv_loads[KRGV_NODES];
static DEFINE_SPINLOCK(krgv_lock);
static u32 krgv_serial;
static u32 krgv_rr;

struct krgv_ticket {
	__u32 ticket;
	__u32 node;
	unsigned long expires;
};

static struct krgv_ticket krgv_tickets[KRGV_TICKETS];

static u32 krgv_self_runq(void)
{
	struct task_struct *p;
	u32 n = 0;

	rcu_read_lock();
	for_each_process(p) {
		if (krgv_state(p) == TASK_RUNNING)
			n++;
	}
	rcu_read_unlock();
	return n;
}

/*
 * Feed one node's numbers in. Rejecting an older seq keeps two collectors from
 * making the table go backwards.
 */
static int krgv_set_load(u32 nid, u32 runq, u32 seq)
{
	if (nid >= KRGV_NODES)
		return -EINVAL;

	spin_lock(&krgv_lock);
	if (!krgv_loads[nid].valid ||
	    (s32)(seq - krgv_loads[nid].seq) > 0) {
		krgv_loads[nid].runq = runq;
		krgv_loads[nid].seq = seq;
		krgv_loads[nid].valid = 1;
	}
	spin_unlock(&krgv_lock);
	return 0;
}

/* Reservations that were never released must not bias placement forever. */
static void krgv_reap_locked(unsigned long now)
{
	int i;

	for (i = 0; i < KRGV_TICKETS; i++) {
		u32 node;

		if (!krgv_tickets[i].ticket)
			continue;
		if (time_before(now, krgv_tickets[i].expires))
			continue;
		node = krgv_tickets[i].node;
		if (node < KRGV_NODES && krgv_loads[node].reserved)
			krgv_loads[node].reserved--;
		krgv_tickets[i].ticket = 0;
	}
}

static int krgv_place(struct krgv_place *out)
{
	unsigned long now = jiffies;
	u32 self = node_id & 0xff;
	u32 self_runq = krgv_self_runq();
	u32 best = self, best_score = ~0u, best_runq = self_runq;
	int slot = -1, k;

	spin_lock(&krgv_lock);
	krgv_reap_locked(now);

	for (k = 0; k < (int)KRGV_NODES; k++) {
		struct krgv_node_load *l;
		u32 idx = (k + krgv_rr) % KRGV_NODES;
		u32 score, runq;

		l = &krgv_loads[idx];
		if (idx == self) {
			runq = self_runq;
		} else {
			if (!l->valid)
				continue;
			runq = l->runq;
		}
		score = runq + l->reserved;
		if (score < best_score) {
			best_score = score;
			best_runq = runq;
			best = idx;
		}
	}

	krgv_rr = (best + 1) % KRGV_NODES;
	krgv_loads[best].reserved++;
	krgv_serial++;

	for (k = 0; k < KRGV_TICKETS; k++) {
		if (!krgv_tickets[k].ticket) {
			slot = k;
			break;
		}
	}
	if (slot < 0) {
		/* table full: evict the entry closest to expiry */
		unsigned long oldest = 0;

		slot = 0;
		for (k = 0; k < KRGV_TICKETS; k++) {
			if (!oldest || time_before(krgv_tickets[k].expires, oldest)) {
				oldest = krgv_tickets[k].expires;
				slot = k;
			}
		}
		if (krgv_loads[krgv_tickets[slot].node].reserved)
			krgv_loads[krgv_tickets[slot].node].reserved--;
	}
	krgv_tickets[slot].ticket = (best << 24) | (krgv_serial & 0x00ffffffu);
	krgv_tickets[slot].node = best;
	krgv_tickets[slot].expires = now + msecs_to_jiffies(KRGV_TTL_MS);

	out->node = best;
	out->ticket = krgv_tickets[slot].ticket;
	out->runq = best_runq;
	out->self = self;
	spin_unlock(&krgv_lock);
	return 0;
}

static void krgv_release(u32 ticket)
{
	u32 nid = ticket >> 24;
	int i;

	if (nid >= KRGV_NODES)
		return;

	spin_lock(&krgv_lock);
	for (i = 0; i < KRGV_TICKETS; i++) {
		if (krgv_tickets[i].ticket != ticket)
			continue;
		krgv_tickets[i].ticket = 0;
		if (krgv_loads[nid].reserved)
			krgv_loads[nid].reserved--;
		break;
	}
	spin_unlock(&krgv_lock);
}

static void krgv_fill(struct task_struct *p, struct krgv_task *t)
{
	char comm[TASK_COMM_LEN];
	struct mm_struct *mm;

	memset(t, 0, sizeof(*t));
	t->pid  = (__u32)p->pid;
	t->tgid = (__u32)p->tgid;
	t->ppid = p->real_parent ? (__u32)p->real_parent->pid : 0;
	t->state = krgv_state(p);
	t->gpid = krgv_gpid((__u32)p->pid);

	get_task_comm(comm, p);
	memcpy(t->comm, comm, sizeof(t->comm));

	mm = get_task_mm(p);
	if (mm) {
		t->rss_kb = (__u32)(get_mm_rss(mm) << (PAGE_SHIFT - 10));
		mmput(mm);
	}
}

static ssize_t krgv_read(struct file *fp, char __user *ubuf, size_t len,
			 loff_t *ppos)
{
	struct krgv_hdr *hdr;
	struct krgv_task *tasks;
	size_t hdr_sz = sizeof(*hdr), rec_sz = sizeof(*tasks);
	size_t need, copy;
	struct task_struct *p;
	int n = 0;
	unsigned int run = 0;

	if (*ppos != 0)
		return 0;
	if (len < hdr_sz)
		return -EINVAL;

	hdr = kvzalloc(hdr_sz, GFP_KERNEL);
	tasks = kvzalloc(rec_sz * KRGV_MAX_TASKS, GFP_KERNEL);
	if (!hdr || !tasks) {
		kvfree(hdr);
		kvfree(tasks);
		return -ENOMEM;
	}

	rcu_read_lock();
	for_each_process(p) {
		if (n >= KRGV_MAX_TASKS)
			break;
		krgv_fill(p, &tasks[n]);
		if (tasks[n].state == TASK_RUNNING)
			run++;
		n++;
	}
	rcu_read_unlock();

	hdr->magic = KRGV_MAGIC;
	hdr->count = (__u32)n;
	hdr->max = KRGV_MAX_TASKS;
	hdr->node_id = (__u32)node_id;
	hdr->run = run;
	{
		struct sysinfo si;

		si_meminfo(&si);
		hdr->mem_total_kb = (__u32)(si.totalram << (PAGE_SHIFT - 10));
		hdr->mem_free_kb = (__u32)(si.freeram << (PAGE_SHIFT - 10));
	}

	need = hdr_sz + (size_t)n * rec_sz;
	copy = min(len, need);

	if (copy_to_user(ubuf, hdr, hdr_sz) ||
	    (n && copy > hdr_sz &&
	     copy_to_user(ubuf + hdr_sz, tasks, copy - hdr_sz))) {
		kvfree(hdr);
		kvfree(tasks);
		return -EFAULT;
	}

	kvfree(hdr);
	kvfree(tasks);
	*ppos = need;
	return (ssize_t)copy;
}

/* "SPAWN <cmd>": let the kernel create the task on this node. */
static int krgv_spawn(const char *cmd)
{
	static char *envp[] = {
		"HOME=/",
		"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		NULL
	};
	char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
	struct subprocess_info *sub;
	int ret;

	sub = call_usermodehelper_setup("/bin/sh", argv, envp, GFP_KERNEL,
					NULL, NULL, NULL);
	if (!sub)
		return -ENOMEM;

	/* WAIT_EXEC: return after exec, leave the task running. */
	ret = call_usermodehelper_exec(sub, UMH_WAIT_EXEC);
	return ret;
}

/* "KILL <gpid>": only tasks owned by this node can be signalled here. */
static int krgv_kill(unsigned long gpid)
{
	unsigned int nid = (gpid >> 24) & 0xff;
	unsigned int pid = gpid & 0x00ffffffu;
	struct task_struct *p;
	int ret = -ESRCH;

	if (nid != (node_id & 0xff))
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

static ssize_t krgv_write(struct file *fp, const char __user *ubuf,
			  size_t len, loff_t *ppos)
{
	char buf[KRGV_CMD_MAX];
	size_t n = min(len, sizeof(buf) - 1);
	int ret;

	if (!n)
		return -EINVAL;

	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;

	if (!strncmp(buf, "SPAWN ", 6)) {
		ret = krgv_spawn(buf + 6);
		return ret ? ret : (ssize_t)len;
	}
	if (!strncmp(buf, "KILL ", 5)) {
		unsigned long gpid;

		ret = kstrtoul(buf + 5, 10, &gpid);
		if (ret)
			return ret;
		ret = krgv_kill(gpid);
		return ret ? ret : (ssize_t)len;
	}
	if (!strncmp(buf, "LOAD ", 5)) {
		unsigned int nid, runq, seq;

		if (sscanf(buf + 5, "%u %u %u", &nid, &runq, &seq) != 3)
			return -EINVAL;
		ret = krgv_set_load(nid, runq, seq);
		return ret ? ret : (ssize_t)len;
	}
	return -EINVAL;
}

static int krgv_open(struct inode *inode, struct file *fp)
{
	return 0;
}

static long krgv_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case KRGV_IOC_PLACE: {
		struct krgv_place p;
		int ret = krgv_place(&p);

		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &p, sizeof(p)))
			return -EFAULT;
		return 0;
	}
	case KRGV_IOC_RELEASE: {
		__u32 ticket;

		if (copy_from_user(&ticket, (void __user *)arg, sizeof(ticket)))
			return -EFAULT;
		krgv_release(ticket);
		return 0;
	}
	case KRGV_IOC_STATE: {
		struct krgv_state *s = kzalloc(sizeof(*s), GFP_KERNEL);
		u32 i, nvalid = 0;

		if (!s)
			return -ENOMEM;
		spin_lock(&krgv_lock);
		s->self = node_id & 0xff;
		for (i = 0; i < KRGV_NODES; i++) {
			s->n[i] = krgv_loads[i];
			if (krgv_loads[i].valid)
				nvalid++;
		}
		for (i = 0; i < KRGV_TICKETS; i++)
			if (krgv_tickets[i].ticket)
				s->tickets_used++;
		s->nvalid = nvalid;
		spin_unlock(&krgv_lock);
		if (copy_to_user((void __user *)arg, s, sizeof(*s))) {
			kfree(s);
			return -EFAULT;
		}
		kfree(s);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations krgv_fops = {
	.owner = THIS_MODULE,
	.open  = krgv_open,
	.read  = krgv_read,
	.write = krgv_write,
	.unlocked_ioctl = krgv_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice krgv_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "krgview",
	.fops  = &krgv_fops,
	.mode  = 0400,
};

static int __init krgv_init(void)
{
	int ret = misc_register(&krgv_dev);

	pr_info("krg_taskview: %s node_id=%u (%d)\n",
		ret ? "register failed" : "loaded", node_id, ret);
	return ret;
}

static void __exit krgv_exit(void)
{
	misc_deregister(&krgv_dev);
	pr_info("krg_taskview: unloaded\n");
}

module_init(krgv_init);
module_exit(krgv_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kerrighed-mini: task table, gpid, remote spawn/kill");
