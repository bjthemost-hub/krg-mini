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
 * needs fault handling and page-level coherence, which is the next step.
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
	unsigned i;

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
			free_seg(segs[i]);
			segs[i] = NULL;
			ret = 0;
			break;
		}
	}
	mutex_unlock(&dsm_lock);
	return ret;
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
	default:
		return -ENOTTY;
	}
}

static const struct file_operations dsm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dsm_ioctl,
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
