// SPDX-License-Identifier: GPL-2.0
/* krg_dsm - distributed shared memory, first step.
 *
 * A segment is a name plus an array of page slots. Nothing is allocated up
 * front: a page costs memory only once somebody actually touches it.
 *
 *   home node   ALLOC a segment, WRITE pages into it, SEAL it
 *   reader      ALLOC the same name with a CACHE limit, pull pages it needs
 *
 * A reader that asks for a cache of N pages holds at most N pages: pulling
 * page N+1 evicts the oldest one. So a node can read a segment far bigger
 * than its own free memory.
 *
 *   /dev/krgdsm ioctl:
 *     ALLOC  reserve a segment of N page slots (no memory yet);
 *            io->cache != 0 makes it a read-only cache of that many pages
 *     WRITE  user -> page  (refused once sealed, refused on a cache segment)
 *     READ   page -> user  (-ENODATA if the page is not resident here)
 *     SEAL   freeze the segment; no further WRITE from userspace
 *     FREE   drop the segment and its pages
 *     INFO   pages / sealed / cache / resident / pages that came from a peer
 *
 * Still an explicit API: a program asks for bytes. Transparent pointer access
 * is the step on top: ATTACH an fd to a segment, mmap it, and a fault for a
 * page that is not here yet is handed to a userspace helper, which pulls the
 * page over the cluster channel and completes the fault. The program just
 * dereferences memory; it never calls read().
 *
 *   KRGDSM_IOC_ATTACH      bind an open fd to a segment (for mmap)
 *   KRGDSM_IOC_FAULT_WAIT  take one pending fault request (blocking, ~200 ms)
 *   KRGDSM_IOC_FAULT_DONE  hand the page back (or report a failure)
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/jiffies.h>

#define KRGDSM_MAX_SEGS		16
#define KRGDSM_MAX_PAGES	262144	/* 1 GiB of slots, not of memory */
#define KRGDSM_NAME		32

struct krgdsm_io {
	char name[KRGDSM_NAME];
	__u32 pages;
	__u32 page;
	__u32 off;
	__u32 len;
	__u64 buf;		/* user pointer */
	__u32 sealed;
	__u32 present;		/* pages that hold valid data here */
	__u32 fetched;		/* pages that came from another node */
	__u32 flags;		/* WRITE: KRGDSM_F_REMOTE */
	__u32 cache;		/* ALLOC: 0 = keep every page, N = bounded */
	__u32 resident;		/* INFO: page slots holding memory now */
};

#define KRGDSM_F_REMOTE		0x1

#define KRGDSM_IOC_MAGIC	'D'
#define KRGDSM_IOC_ALLOC	_IOWR(KRGDSM_IOC_MAGIC, 1, struct krgdsm_io)
#define KRGDSM_IOC_WRITE	_IOW(KRGDSM_IOC_MAGIC, 2, struct krgdsm_io)
#define KRGDSM_IOC_READ		_IOWR(KRGDSM_IOC_MAGIC, 3, struct krgdsm_io)
#define KRGDSM_IOC_SEAL		_IOW(KRGDSM_IOC_MAGIC, 4, struct krgdsm_io)
#define KRGDSM_IOC_FREE		_IOW(KRGDSM_IOC_MAGIC, 5, struct krgdsm_io)
#define KRGDSM_IOC_INFO		_IOWR(KRGDSM_IOC_MAGIC, 6, struct krgdsm_io)
#define KRGDSM_IOC_ATTACH	_IOW(KRGDSM_IOC_MAGIC, 7, struct krgdsm_io)
#define KRGDSM_IOC_FAULT_WAIT	_IOWR(KRGDSM_IOC_MAGIC, 8, struct krgdsm_io)
#define KRGDSM_IOC_FAULT_DONE	_IOW(KRGDSM_IOC_MAGIC, 9, struct krgdsm_io)

#define KRGDSM_F_FAULT_ERR	0x2	/* FAULT_DONE: could not fetch */
#define KRGDSM_FAULT_RETAKE	(15 * HZ)	/* a helper that died */

struct dsm_req {
	struct list_head node;
	unsigned page;
	unsigned long taken_at;	/* 0 = nobody has picked it up */
};

struct krgdsm_seg {
	char name[KRGDSM_NAME];
	unsigned pages;
	unsigned sealed;
	unsigned fetched;
	unsigned cache;		/* 0 = unbounded */
	unsigned resident;
	struct page **pgs;
	u8 *present;
	unsigned *fifo;		/* ring of resident page indices, size cache */
	unsigned head, len;
	unsigned refs;		/* open fds bound to this segment */
	unsigned mapped;	/* live vmas */
	unsigned gen;		/* bumped on every completed fault */
	struct list_head reqs;	/* pending fault requests */
	wait_queue_head_t req_wq;
	wait_queue_head_t done_wq;
};

static struct krgdsm_seg *segs[KRGDSM_MAX_SEGS];
static DEFINE_MUTEX(dsm_lock);

static struct krgdsm_seg *find_seg(const char *name)
{
	int i;

	for (i = 0; i < KRGDSM_MAX_SEGS; i++) {
		if (segs[i] && !strncmp(segs[i]->name, name, KRGDSM_NAME))
			return segs[i];
	}
	return NULL;
}

/* drop one page slot; only pages fetched from a peer are ever evicted, so a
 * node never loses data it is the home of. */
static void drop_page(struct krgdsm_seg *s, unsigned p)
{
	if (!s->pgs[p])
		return;
	__free_page(s->pgs[p]);
	s->pgs[p] = NULL;
	s->present[p] = 0;
	s->resident--;
}

static struct krgdsm_seg *alloc_seg(const char *name, unsigned pages,
				    unsigned cache)
{
	struct krgdsm_seg *s;

	if (!pages || pages > KRGDSM_MAX_PAGES)
		return ERR_PTR(-EINVAL);
	if (cache > pages)
		return ERR_PTR(-EINVAL);

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);
	memcpy(s->name, name, KRGDSM_NAME);
	s->name[KRGDSM_NAME - 1] = 0;
	s->pages = pages;
	s->cache = cache;
	INIT_LIST_HEAD(&s->reqs);
	init_waitqueue_head(&s->req_wq);
	init_waitqueue_head(&s->done_wq);

	s->pgs = kvcalloc(pages, sizeof(*s->pgs), GFP_KERNEL);
	s->present = kvcalloc(pages, 1, GFP_KERNEL);
	if (cache)
		s->fifo = kcalloc(cache, sizeof(*s->fifo), GFP_KERNEL);

	if (!s->pgs || !s->present || (cache && !s->fifo)) {
		kvfree(s->pgs);
		kvfree(s->present);
		kfree(s->fifo);
		kfree(s);
		return ERR_PTR(-ENOMEM);
	}
	return s;		/* no pages allocated: slots only */
}

static void free_seg(struct krgdsm_seg *s)
{
	struct dsm_req *r, *tmp;
	unsigned i;

	list_for_each_entry_safe(r, tmp, &s->reqs, node) {
		list_del(&r->node);
		kfree(r);
	}
	for (i = 0; i < s->pages; i++)
		if (s->pgs[i])
			__free_page(s->pgs[i]);
	kvfree(s->pgs);
	kvfree(s->present);
	kfree(s->fifo);
	kfree(s);
}

static int dsm_alloc(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	int slot = -1, i;

	mutex_lock(&dsm_lock);
	if (find_seg(io->name)) {
		mutex_unlock(&dsm_lock);
		return -EEXIST;
	}
	for (i = 0; i < KRGDSM_MAX_SEGS; i++) {
		if (!segs[i]) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		mutex_unlock(&dsm_lock);
		return -ENOSPC;
	}
	s = alloc_seg(io->name, io->pages, io->cache);
	if (IS_ERR(s)) {
		int ret = PTR_ERR(s);

		mutex_unlock(&dsm_lock);
		return ret;
	}
	segs[slot] = s;
	mutex_unlock(&dsm_lock);
	return 0;
}

/* make room for one more page, evicting the oldest cached page if needed */
static int make_room(struct krgdsm_seg *s)
{
	if (!s->cache)
		return 0;
	if (s->mapped)
		return -ENOMEM;	/* pages handed to a vma cannot be pulled back */
	while (s->resident >= s->cache && s->len) {
		drop_page(s, s->fifo[s->head]);
		s->head = (s->head + 1) % s->cache;
		s->len--;
	}
	if (s->resident >= s->cache)
		return -ENOMEM;
	return 0;
}

static void note_cached(struct krgdsm_seg *s, unsigned p)
{
	if (!s->cache)
		return;
	s->fifo[(s->head + s->len) % s->cache] = p;
	s->len++;
}

static int dsm_write(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	char *kaddr;
	int remote = !!(io->flags & KRGDSM_F_REMOTE);
	int ret = 0;

	mutex_lock(&dsm_lock);
	s = find_seg(io->name);
	if (!s) {
		ret = -ENOENT;
		goto out;
	}
	if (s->sealed) {
		ret = -EPERM;
		goto out;
	}
	if (s->cache && !remote) {
		ret = -EPERM;	/* a cache takes copies, it does not own data */
		goto out;
	}
	if (io->page >= s->pages || io->off > PAGE_SIZE ||
	    io->len > PAGE_SIZE - io->off) {
		ret = -EINVAL;
		goto out;
	}

	if (!s->pgs[io->page]) {
		struct page *pg;

		if (s->cache) {
			ret = make_room(s);
			if (ret)
				goto out;
		}
		pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pg) {
			ret = -ENOMEM;
			goto out;
		}
		s->pgs[io->page] = pg;
		s->resident++;
	}

	kaddr = kmap_local_page(s->pgs[io->page]);
	if (copy_from_user(kaddr + io->off, (void __user *)(uintptr_t)io->buf,
			   io->len)) {
		kunmap_local(kaddr);
		ret = -EFAULT;
		goto out;
	}
	kunmap_local(kaddr);

	if (!s->present[io->page]) {
		s->present[io->page] = 1;
		if (remote) {
			s->fetched++;
			note_cached(s, io->page);
		}
	}
out:
	mutex_unlock(&dsm_lock);
	return ret;
}

static int dsm_read(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	char *kaddr;
	int ret = 0;

	mutex_lock(&dsm_lock);
	s = find_seg(io->name);
	if (!s) {
		ret = -ENOENT;
		goto out;
	}
	if (io->page >= s->pages || io->off > PAGE_SIZE ||
	    io->len > PAGE_SIZE - io->off) {
		ret = -EINVAL;
		goto out;
	}
	if (!s->pgs[io->page]) {
		ret = -ENODATA;	/* not here: fetch it from its home */
		goto out;
	}

	kaddr = kmap_local_page(s->pgs[io->page]);
	if (copy_to_user((void __user *)(uintptr_t)io->buf, kaddr + io->off,
			 io->len))
		ret = -EFAULT;
	kunmap_local(kaddr);
out:
	mutex_unlock(&dsm_lock);
	return ret;
}

static int dsm_seal(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	int ret = 0;

	mutex_lock(&dsm_lock);
	s = find_seg(io->name);
	if (!s)
		ret = -ENOENT;
	else
		s->sealed = 1;
	mutex_unlock(&dsm_lock);
	return ret;
}

static int dsm_free(struct krgdsm_io *io)
{
	int i, ret = -ENOENT;

	mutex_lock(&dsm_lock);
	for (i = 0; i < KRGDSM_MAX_SEGS; i++) {
		if (segs[i] && !strncmp(segs[i]->name, io->name, KRGDSM_NAME)) {
			if (segs[i]->refs || segs[i]->mapped) {
				ret = -EBUSY;
				break;
			}
			free_seg(segs[i]);
			segs[i] = NULL;
			ret = 0;
			break;
		}
	}
	mutex_unlock(&dsm_lock);
	return ret;
}

/* ---- fault forwarding: a page the vma wants is not here yet -------------- */

static struct dsm_req *find_req(struct krgdsm_seg *s, unsigned page)
{
	struct dsm_req *r;

	list_for_each_entry(r, &s->reqs, node)
		if (r->page == page)
			return r;
	return NULL;
}

static void add_req(struct krgdsm_seg *s, unsigned page)
{
	struct dsm_req *r = kmalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return;
	r->page = page;
	r->taken_at = 0;
	list_add_tail(&r->node, &s->reqs);
}

static int dsm_attach(struct file *fp, struct krgdsm_io *io)
{
	struct krgdsm_seg *s;

	if (fp->private_data)
		return -EBUSY;
	mutex_lock(&dsm_lock);
	s = find_seg(io->name);
	if (s)
		s->refs++;
	mutex_unlock(&dsm_lock);
	if (!s)
		return -ENOENT;
	fp->private_data = s;
	return 0;
}

static int dsm_release(struct inode *ino, struct file *fp)
{
	struct krgdsm_seg *s = fp->private_data;

	if (s) {
		mutex_lock(&dsm_lock);
		if (s->refs)
			s->refs--;
		mutex_unlock(&dsm_lock);
		fp->private_data = NULL;
	}
	return 0;
}

/* misc_open() stuffs the struct miscdevice into private_data; we want it
 * empty so that ATTACH can put a segment there. */
static int dsm_open(struct inode *ino, struct file *fp)
{
	fp->private_data = NULL;
	return 0;
}

/* helper side: take one request, or time out so the helper can check a flag */
static int dsm_fault_wait(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	struct dsm_req *r;
	int ret;

	/* never sleep with dsm_lock held: the fault side needs it to post */
	for (;;) {
		mutex_lock(&dsm_lock);
		s = find_seg(io->name);
		if (!s) {
			mutex_unlock(&dsm_lock);
			return -ENOENT;
		}
		list_for_each_entry(r, &s->reqs, node) {
			int free_slot = !r->taken_at ||
				time_after(jiffies,
					   r->taken_at + KRGDSM_FAULT_RETAKE);

			if (!free_slot)
				continue;
			r->taken_at = jiffies ? jiffies : 1;
			io->page = r->page;
			mutex_unlock(&dsm_lock);
			return 0;
		}
		mutex_unlock(&dsm_lock);

		ret = wait_event_interruptible_timeout(s->req_wq,
						       !list_empty(&s->reqs),
						       HZ / 5);
		if (ret < 0)
			return ret;
		if (ret == 0)
			return -EAGAIN;
	}
}

/* helper side: the page (or a failure) comes back */
static int dsm_fault_done(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	struct dsm_req *r;
	struct page *pg;
	char *kaddr;
	int ret = -ENOENT;

	mutex_lock(&dsm_lock);
	s = find_seg(io->name);
	if (!s)
		goto out;
	if (io->page >= s->pages) {
		ret = -EINVAL;
		goto out;
	}
	r = find_req(s, io->page);
	if (!r) {
		ret = -ENOENT;
		goto out;
	}

	if (!(io->flags & KRGDSM_F_FAULT_ERR) && !s->pgs[io->page]) {
		pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pg) {
			ret = -ENOMEM;
			goto out;
		}
		kaddr = kmap_local_page(pg);
		if (copy_from_user(kaddr, (void __user *)(uintptr_t)io->buf,
				   min_t(unsigned, io->len, PAGE_SIZE))) {
			kunmap_local(kaddr);
			__free_page(pg);
			ret = -EFAULT;
			goto out;
		}
		kunmap_local(kaddr);
		s->pgs[io->page] = pg;
		s->resident++;
	}
	if (!(io->flags & KRGDSM_F_FAULT_ERR))
		s->present[io->page] = 1;

	list_del(&r->node);
	kfree(r);
	s->gen++;
	wake_up_all(&s->done_wq);
	ret = 0;
out:
	mutex_unlock(&dsm_lock);
	return ret;
}

static vm_fault_t dsm_vm_fault(struct vm_fault *vmf)
{
	struct krgdsm_seg *s = vmf->vma->vm_file->private_data;
	unsigned idx = vmf->pgoff;
	unsigned gen;
	int i;

	if (!s || idx >= s->pages)
		return VM_FAULT_SIGBUS;

	for (i = 0; i < 300; i++) {		/* ~30 s, then give up */
		struct page *pg = NULL;

		mutex_lock(&dsm_lock);
		if (s->present[idx] && s->pgs[idx]) {
			pg = s->pgs[idx];
			get_page(pg);
			mutex_unlock(&dsm_lock);
			vmf->page = pg;
			return 0;
		}
		if (!find_req(s, idx)) {
			add_req(s, idx);
			wake_up_all(&s->req_wq);
		}
		gen = s->gen;
		mutex_unlock(&dsm_lock);

		if (wait_event_interruptible_timeout(s->done_wq, s->gen != gen,
						     HZ / 10) < 0)
			return VM_FAULT_SIGBUS;
	}
	return VM_FAULT_SIGBUS;
}

static void dsm_vm_open(struct vm_area_struct *vma)
{
	struct krgdsm_seg *s = vma->vm_file->private_data;

	mutex_lock(&dsm_lock);
	if (s) {
		s->mapped++;
		try_module_get(THIS_MODULE);
	}
	mutex_unlock(&dsm_lock);
}

static void dsm_vm_close(struct vm_area_struct *vma)
{
	struct krgdsm_seg *s = vma->vm_file->private_data;

	mutex_lock(&dsm_lock);
	if (s && s->mapped)
		s->mapped--;
	mutex_unlock(&dsm_lock);
	module_put(THIS_MODULE);
}

static const struct vm_operations_struct dsm_vm_ops = {
	.open	= dsm_vm_open,
	.close	= dsm_vm_close,
	.fault	= dsm_vm_fault,
};

static int dsm_mmap(struct file *fp, struct vm_area_struct *vma)
{
	struct krgdsm_seg *s = fp->private_data;
	unsigned long n;

	if (!s)
		return -EINVAL;
	if (vma->vm_pgoff >= s->pages)
		return -EINVAL;
	n = vma->vm_end - vma->vm_start;
	if (vma->vm_pgoff + (n >> PAGE_SHIFT) > s->pages)
		return -EINVAL;

	vma->vm_ops = &dsm_vm_ops;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	dsm_vm_open(vma);
	return 0;
}

static int dsm_info(struct krgdsm_io *io)
{
	struct krgdsm_seg *s;
	unsigned i, present = 0;
	int ret = 0;

	mutex_lock(&dsm_lock);
	s = find_seg(io->name);
	if (!s) {
		ret = -ENOENT;
		goto out;
	}
	for (i = 0; i < s->pages; i++)
		if (s->present[i])
			present++;
	io->pages = s->pages;
	io->sealed = s->sealed;
	io->present = present;
	io->fetched = s->fetched;
	io->cache = s->cache;
	io->resident = s->resident;
out:
	mutex_unlock(&dsm_lock);
	return ret;
}

static long dsm_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct krgdsm_io io;
	int ret;

	if (copy_from_user(&io, (void __user *)arg, sizeof(io)))
		return -EFAULT;
	io.name[KRGDSM_NAME - 1] = 0;

	switch (cmd) {
	case KRGDSM_IOC_ALLOC:
		return dsm_alloc(&io);
	case KRGDSM_IOC_WRITE:
		return dsm_write(&io);
	case KRGDSM_IOC_READ:
		return dsm_read(&io);
	case KRGDSM_IOC_SEAL:
		return dsm_seal(&io);
	case KRGDSM_IOC_FREE:
		return dsm_free(&io);
	case KRGDSM_IOC_INFO:
		ret = dsm_info(&io);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &io, sizeof(io)))
			return -EFAULT;
		return 0;
	case KRGDSM_IOC_ATTACH:
		return dsm_attach(fp, &io);
	case KRGDSM_IOC_FAULT_WAIT:
		ret = dsm_fault_wait(&io);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &io, sizeof(io)))
			return -EFAULT;
		return 0;
	case KRGDSM_IOC_FAULT_DONE:
		return dsm_fault_done(&io);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dsm_fops = {
	.owner = THIS_MODULE,
	.open = dsm_open,
	.unlocked_ioctl = dsm_ioctl,
	.mmap = dsm_mmap,
	.release = dsm_release,
	.llseek = noop_llseek,
};

static struct miscdevice dsm_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "krgdsm",
	.fops = &dsm_fops,
	.mode = 0600,
};

static int __init dsm_init(void)
{
	int ret = misc_register(&dsm_dev);

	pr_info("krg_dsm: %s (%d)\n", ret ? "register failed" : "loaded", ret);
	return ret;
}

static void __exit dsm_exit(void)
{
	int i;

	mutex_lock(&dsm_lock);
	for (i = 0; i < KRGDSM_MAX_SEGS; i++)
		if (segs[i]) {
			free_seg(segs[i]);
			segs[i] = NULL;
		}
	mutex_unlock(&dsm_lock);
	misc_deregister(&dsm_dev);
	pr_info("krg_dsm: unloaded\n");
}

module_init(dsm_init);
module_exit(dsm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kerrighed-mini: paged shared segments, pages pulled on demand");
