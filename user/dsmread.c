// SPDX-License-Identifier: GPL-2.0
/* dsmread - read a shared segment as memory, with no read() calls.
 *
 *   dsmread NAME HOST:PORT [--len BYTES] [--quiet]
 *
 * The segment must already exist on this node (krgdsm alloc NAME PAGES).
 * We bind an fd to it, mmap it read-only, and let a helper thread answer the
 * page faults: a fault for a page that is not here yet is handed to us by the
 * kernel, we pull that page from the peer with DSMGET, and hand it back. The
 * main thread only walks a char pointer.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DSM_DEV		"/dev/krgdsm"
#define DSM_NAME	32
#define PAGE_SIZE_	4096

struct krgdsm_io {
	char name[DSM_NAME];
	unsigned pages, page, off, len;
	unsigned long buf;
	unsigned sealed, present, fetched, flags;
	unsigned cache, resident;
};

#define KRGDSM_IOC_MAGIC	'D'
#define KRGDSM_IOC_ATTACH	_IOW(KRGDSM_IOC_MAGIC, 7, struct krgdsm_io)
#define KRGDSM_IOC_FAULT_WAIT	_IOWR(KRGDSM_IOC_MAGIC, 8, struct krgdsm_io)
#define KRGDSM_IOC_FAULT_DONE	_IOW(KRGDSM_IOC_MAGIC, 9, struct krgdsm_io)
#define KRGDSM_IOC_INFO		_IOWR(KRGDSM_IOC_MAGIC, 6, struct krgdsm_io)
#define KRGDSM_IOC_WRITE	_IOW(KRGDSM_IOC_MAGIC, 2, struct krgdsm_io)
#define KRGDSM_F_FAULT_ERR	0x2
#define KRGDSM_F_REMOTE		0x1

#define PREFETCH	64	/* one round trip brings this many pages */

static int		dev_fd = -1;
static const char      *seg_name;
static char		peer_host[256];
static int		peer_port;
static volatile int	running = 1;
static unsigned long	faults, fetch_bytes, fetch_fail;
static double		net_seconds;
static unsigned		seg_pages;

static int ioctl_bind(int fd, unsigned long cmd, struct krgdsm_io *io)
{
	return ioctl(fd, cmd, io);
}

/* pull one page from the peer; returns bytes, or -1 */
static int fetch_pages(unsigned page, void *buf, unsigned count)
{
	struct sockaddr_in sa;
	char hdr[128];
	int s, n, len, got;
	struct timeval tv = { 3, 0 };
	size_t off = 0;
	char *p;

	if (peer_port <= 0)
		return -1;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(peer_port);
	if (inet_pton(AF_INET, peer_host, &sa.sin_addr) != 1) {
		close(s);
		return -1;
	}
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(s);
		return -1;
	}
	len = snprintf(hdr, sizeof(hdr), "DSMGET %s %u %u\n", seg_name, page,
		       count);
	if (write(s, hdr, len) != len) {
		close(s);
		return -1;
	}
	got = 0;
	while (got < (int)sizeof(hdr) - 1) {
		n = read(s, hdr + got, 1);
		if (n <= 0)
			break;
		if (hdr[got] == '\n') {
			got++;
			break;
		}
		got++;
	}
	hdr[got] = 0;
	if (strncmp(hdr, "OK ", 3)) {
		close(s);
		return -1;
	}
	n = atoi(hdr + 3);
	if (n <= 0 || n > (int)count * PAGE_SIZE_) {
		close(s);
		return -1;
	}
	p = buf;
	while (off < (size_t)n) {
		ssize_t r = read(s, p + off, n - off);

		if (r <= 0) {
			close(s);
			return -1;
		}
		off += r;
	}
	close(s);
	return n;
}

static void *helper(void *arg)
{
	struct krgdsm_io io;
	static char page_buf[PREFETCH * PAGE_SIZE_];
	struct timespec t0, t1;

	(void)arg;
	while (running) {
		unsigned pg;
		unsigned cnt, i;
		int n;

		memset(&io, 0, sizeof(io));
		strncpy(io.name, seg_name, DSM_NAME - 1);
		if (ioctl_bind(dev_fd, KRGDSM_IOC_FAULT_WAIT, &io) != 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}
		pg = io.page;
		cnt = PREFETCH;
		if (pg + cnt > seg_pages)
			cnt = seg_pages - pg;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		n = fetch_pages(pg, page_buf, cnt);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		net_seconds += (t1.tv_sec - t0.tv_sec) +
			       (t1.tv_nsec - t0.tv_nsec) / 1e9;

		/* deposit every prefetched page, so the next faults are hits */
		for (i = 1; n > 0 && i < cnt && pg + i < seg_pages; i++) {
			struct krgdsm_io d;

			if ((i + 1) * PAGE_SIZE_ > (size_t)n)
				break;
			memset(&d, 0, sizeof(d));
			strncpy(d.name, seg_name, DSM_NAME - 1);
			d.page = pg + i;
			d.buf = (unsigned long)(page_buf + i * PAGE_SIZE_);
			d.len = PAGE_SIZE_;
			d.flags = KRGDSM_F_REMOTE;
			ioctl_bind(dev_fd, KRGDSM_IOC_WRITE, &d);
		}

		memset(&io, 0, sizeof(io));
		strncpy(io.name, seg_name, DSM_NAME - 1);
		io.page = pg;
		if (n > 0) {
			io.buf = (unsigned long)page_buf;
			io.len = (unsigned)n;
			fetch_bytes += n;
		} else {
			io.flags = KRGDSM_F_FAULT_ERR;
			fetch_fail++;
		}
		if (ioctl_bind(dev_fd, KRGDSM_IOC_FAULT_DONE, &io) == 0)
			faults++;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct krgdsm_io io;
	struct timespec t0, t1;
	pthread_t th;
	size_t len, i;
	volatile unsigned long sum = 0;
	const unsigned char *base;
	unsigned bad = 0;
	double el;
	char *colon;

	if (argc < 3) {
		fprintf(stderr, "usage: %s NAME HOST:PORT [--len BYTES]\n", argv[0]);
		return 2;
	}
	seg_name = argv[1];
	strncpy(peer_host, argv[2], sizeof(peer_host) - 1);
	colon = strrchr(peer_host, ':');
	if (!colon) {
		fprintf(stderr, "peer must be HOST:PORT\n");
		return 2;
	}
	*colon = 0;
	peer_port = atoi(colon + 1);

	len = 0;
	for (i = 3; i < (size_t)argc; i++)
		if (!strcmp(argv[i], "--len") && i + 1 < (size_t)argc)
			len = strtoul(argv[++i], NULL, 0);

	dev_fd = open(DSM_DEV, O_RDWR);
	if (dev_fd < 0) {
		perror(DSM_DEV);
		return 1;
	}
	memset(&io, 0, sizeof(io));
	strncpy(io.name, seg_name, DSM_NAME - 1);
	if (ioctl_bind(dev_fd, KRGDSM_IOC_ATTACH, &io) != 0) {
		perror("attach");
		return 1;
	}
	memset(&io, 0, sizeof(io));
	strncpy(io.name, seg_name, DSM_NAME - 1);
	if (ioctl_bind(dev_fd, KRGDSM_IOC_INFO, &io) != 0) {
		perror("info");
		return 1;
	}
	seg_pages = io.pages;
	if (!len)
		len = (size_t)io.pages * PAGE_SIZE_;
	if (len > (size_t)io.pages * PAGE_SIZE_)
		len = (size_t)io.pages * PAGE_SIZE_;

	base = mmap(NULL, len, PROT_READ, MAP_SHARED, dev_fd, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	if (pthread_create(&th, NULL, helper, NULL)) {
		perror("pthread_create");
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < len; i += PAGE_SIZE_) {
		char want[64];
		unsigned pg = i / PAGE_SIZE_;

		sum += base[i];
		memset(want, 0, sizeof(want));
		snprintf(want, sizeof(want), "krgdsm page %u", pg);
		if (memcmp(base + i, want, strlen(want) + 1))
			bad++;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	running = 0;
	pthread_join(th, NULL);
	munmap((void *)base, len);
	close(dev_fd);

	printf("%s: %zu MiB as memory in %.3f s: %.2f MiB/s, "
	       "faults %lu, fetched %.1f MiB, fetch failures %lu, "
	       "mismatched pages %u (sum %lu)\n",
	       seg_name, len >> 20, el, (len / 1048576.0) / el,
	       faults, fetch_bytes / 1048576.0, fetch_fail, bad, sum);
	return bad ? 1 : 0;
}
