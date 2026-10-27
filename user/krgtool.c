/* krgtool - Kerrighed-mini userspace side.
 *
 *   krgtool local                 print this node's task table
 *   krgtool serve [port]          serve this node + SPAWN/KILL forwarding
 *   krgtool show peer:port,...    merge local + remote nodes into one table
 *   krgtool spawn peer:port cmd   ask that node's kernel to create a task
 *   krgtool kill  peer:port gpid  ask that node's kernel to kill a task
 *   krgtool spawn --auto [--nodes FILE] cmd
 *                                 let the LOCAL KERNEL pick the node, then run there
 *   krgtool place                 print the node the kernel would pick now
 *   krgtool release ticket        give a reservation back
 *   krgtool state                 dump the load table the kernel is using
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <time.h>

#define DEV		"/dev/krgview"
#define KRGV_MAGIC	0x4b524779u
#define KRGV_MAX	4096

struct krgv_hdr {
	unsigned magic, count, max, node_id, run, pad, mem_total_kb, mem_free_kb;
};

struct krgv_task {
	unsigned pid, tgid, ppid, state, rss_kb, gpid;
	char comm[16];
};

/* ---- placement, mirrored from module/krg_taskview.c --------------------- */

#define KRGV_NODES	256

struct krgv_node_load {
	unsigned runq, seq, reserved, valid;
};

struct krgv_place {
	unsigned node, ticket, runq, self;
};

struct krgv_state {
	unsigned self, nvalid, tickets_used, pad;
	struct krgv_node_load n[KRGV_NODES];
};

#define KRGV_IOC_MAGIC		'K'
#define KRGV_IOC_PLACE		_IOR(KRGV_IOC_MAGIC, 1, struct krgv_place)
#define KRGV_IOC_RELEASE	_IOW(KRGV_IOC_MAGIC, 2, unsigned)
#define KRGV_IOC_STATE		_IOR(KRGV_IOC_MAGIC, 3, struct krgv_state)

static const char *st_name(unsigned s)
{
	switch (s & 0xff) {
	case 0:   return "R";
	case 1:   return "S";
	case 2:   return "D";
	case 4:   return "T";
	case 8:   return "t";
	case 16:  return "X";
	case 32:  return "Z";
	case 64:  return "I";
	case 128: return "I";
	case 256: return "P";
	default:  return "?";
	}
}

static void free_rec(const void *p);

static int read_local_ex(struct krgv_task **out, unsigned *count,
			 unsigned *node_id, unsigned *run, unsigned *mtotal,
			 unsigned *mfree)
{
	size_t cap = sizeof(struct krgv_hdr) + sizeof(struct krgv_task) * KRGV_MAX;
	struct krgv_hdr *h;
	char *base;
	ssize_t n;
	int fd;

	fd = open(DEV, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return -1;
	}

	base = malloc(cap);
	if (!base) {
		close(fd);
		return -1;
	}

	n = read(fd, base, cap);
	close(fd);
	if (n < (ssize_t)sizeof(*h)) {
		fprintf(stderr, "read %s: %s\n", DEV,
			n < 0 ? strerror(errno) : "short read");
		free(base);
		return -1;
	}

	h = (struct krgv_hdr *)base;
	if (h->magic != KRGV_MAGIC) {
		fprintf(stderr, "%s: bad magic %08x (module v1? reinstall)\n",
			DEV, h->magic);
		free(base);
		return -1;
	}

	*out = (struct krgv_task *)(base + sizeof(struct krgv_hdr));
	*count = h->count;
	*node_id = h->node_id;
	if (run)
		*run = h->run;
	if (mtotal)
		*mtotal = h->mem_total_kb;
	if (mfree)
		*mfree = h->mem_free_kb;
	return 0;	/* caller frees the base pointer via free_rec() */
}

static int read_local(struct krgv_task **out, unsigned *count)
{
	unsigned nid, run;
	return read_local_ex(out, count, &nid, &run, NULL, NULL);
}

static int local_load(unsigned *node_id, unsigned *count, unsigned *run)
{
	struct krgv_task *t;

	if (read_local_ex(&t, count, node_id, run, NULL, NULL) < 0)
		return -1;
	free_rec(t);
	return 0;
}

static int local_mem(unsigned *node_id, unsigned *mtotal, unsigned *mfree)
{
	struct krgv_task *t;
	unsigned cnt, run;

	if (read_local_ex(&t, &cnt, node_id, &run, mtotal, mfree) < 0)
		return -1;
	free_rec(t);
	return 0;
}

static void free_rec(const void *p) { free((char *)p - sizeof(struct krgv_hdr)); }

/* ask the local kernel to spawn / kill */
static int local_write_cmd(const char *buf, size_t len)
{
	int fd = open(DEV, O_WRONLY);
	ssize_t w;

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return -1;
	}
	w = write(fd, buf, len);
	close(fd);
	if (w != (ssize_t)len) {
		fprintf(stderr, "write %s: %s\n", DEV,
			w < 0 ? strerror(errno) : "short write");
		return -1;
	}
	return 0;
}

static void print_rows(const char *node, const struct krgv_task *t, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		printf("%-10s %8u %6u %6u %-2s %8u %8u  %s\n",
		       node, t[i].gpid, t[i].pid, t[i].ppid, st_name(t[i].state),
		       t[i].tgid, t[i].rss_kb, t[i].comm);
}

static int show_local(const char *node)
{
	struct krgv_task *t;
	unsigned n;

	if (read_local(&t, &n) < 0)
		return -1;
	print_rows(node, t, n);
	free_rec(t);
	return 0;
}

static int split_peer(const char *peer, char *host, size_t hlen, char *port, size_t plen)
{
	const char *c = strrchr(peer, ':');

	if (!c)
		return -1;
	if ((size_t)(c - peer) >= hlen)
		return -1;
	memcpy(host, peer, c - peer);
	host[c - peer] = 0;
	snprintf(port, plen, "%s", c + 1);
	return 0;
}

static int connect_node(const char *peer, char *host, size_t hlen)
{
	struct addrinfo hints, *res;
	char port[16];
	int fd, rc;

	if (split_peer(peer, host, hlen, port, sizeof(port)) < 0)
		return -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(host, port, &hints, &res);
	if (rc) {
		fprintf(stderr, "%s: %s\n", peer, gai_strerror(rc));
		return -1;
	}

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		fprintf(stderr, "%s: connect: %s\n", peer, strerror(errno));
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int show_peer(const char *peer)
{
	char host[128], line[512];
	FILE *fp;
	int fd;

	fd = connect_node(peer, host, sizeof(host));
	if (fd < 0)
		return -1;

	if (write(fd, "LIST\n", 5) != 5) {
		close(fd);
		return -1;
	}

	fp = fdopen(fd, "r");
	if (!fp) {
		close(fd);
		return -1;
	}

	printf("# node %s (%s)\n", host, peer);
	while (fgets(line, sizeof(line), fp)) {
		unsigned pid, ppid, tgid, rss, gpid;
		char st[8], comm[64];

		if (!strncmp(line, "END", 3))
			break;
		if (sscanf(line, "# load %u %u", &tgid, &rss) == 2) {
			printf("# load run=%u tasks=%u\n", tgid, rss);
			continue;
		}
		if (sscanf(line, "%u %u %7s %u %u %u %63[^\n]",
			   &pid, &ppid, st, &tgid, &rss, &gpid, comm) == 7)
			printf("%-10s %8u %6u %6u %-2s %8u %8u  %s\n",
			       host, gpid, pid, ppid, st, tgid, rss, comm);
	}
	fclose(fp);
	return 0;
}

/* run a command on a peer and stream its output until END */
static int remote_exec(const char *peer, const char *cmd)
{
	char host[128], buf[1024];
	ssize_t r;
	int fd;

	fd = connect_node(peer, host, sizeof(host));
	if (fd < 0)
		return -1;
	if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
		close(fd);
		return -1;
	}
	while ((r = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[r] = 0;
		if (r >= 4 && !strcmp(buf + r - 4, "END\n")) {
			buf[r - 4] = 0;
			fputs(buf, stdout);
			break;
		}
		fputs(buf, stdout);
	}
	close(fd);
	return 0;
}

/* send a one-line command to a peer and print its reply */
static int remote_cmd(const char *peer, const char *cmd)
{
	char host[128], buf[1024];
	ssize_t r;
	int fd;

	fd = connect_node(peer, host, sizeof(host));
	if (fd < 0)
		return -1;
	if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
		close(fd);
		return -1;
	}
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = 0;
	fputs(buf, stdout);
	return strncmp(buf, "OK", 2) ? -1 : 0;
}

/* same, but hand the reply back instead of printing it */
static int remote_cmd_buf(const char *peer, const char *cmd, char *out, size_t len)
{
	char host[128];
	ssize_t r;
	int fd;

	fd = connect_node(peer, host, sizeof(host));
	if (fd < 0)
		return -1;
	if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
		close(fd);
		return -1;
	}
	r = read(fd, out, len - 1);
	close(fd);
	if (r <= 0)
		return -1;
	out[r] = 0;
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

/* ---- placement ---------------------------------------------------------- */

static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

/* ask one peer for its run queue, and put it into the local kernel's table */
static int collect_one(const char *peer, unsigned *nid_out, unsigned *run_out)
{
	char rep[256], cmd[128];
	unsigned nid, run, cnt;
	ssize_t w;
	int fd;

	if (remote_cmd_buf(peer, "LOAD\n", rep, sizeof(rep)) < 0) {
		fprintf(stderr, "%s: no reply\n", peer);
		return -1;
	}
	if (sscanf(rep, "OK %u %u %u", &nid, &run, &cnt) != 3) {
		fprintf(stderr, "%s: bad reply '%s'\n", peer, rep);
		return -1;
	}

	fd = open(DEV, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return -1;
	}
	snprintf(cmd, sizeof(cmd), "LOAD %u %u %u\n", nid, run, now_ms());
	w = write(fd, cmd, strlen(cmd));
	close(fd);
	if (w != (ssize_t)strlen(cmd)) {
		fprintf(stderr, "write %s: %s\n", DEV,
			w < 0 ? strerror(errno) : "short write");
		return -1;
	}

	*nid_out = nid;
	*run_out = run;
	return 0;
}

static int kernel_place(struct krgv_place *p)
{
	int fd = open(DEV, O_RDONLY);
	int ret;

	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return -1;
	}
	ret = ioctl(fd, KRGV_IOC_PLACE, p);
	if (ret < 0)
		fprintf(stderr, "PLACE: %s (module too old? reinstall)\n",
			strerror(errno));
	close(fd);
	return ret < 0 ? -1 : 0;
}

static void kernel_release(unsigned ticket)
{
	int fd = open(DEV, O_RDONLY);

	if (fd < 0)
		return;
	ioctl(fd, KRGV_IOC_RELEASE, &ticket);
	close(fd);
}

/* every address in the node list, plus its node id as the node itself reports it */
struct peer_nid {
	char addr[160];
	unsigned nid;
};

static int load_node_list(const char *path, struct peer_nid *peers, int max)
{
	FILE *fp = fopen(path, "r");
	char line[256];
	int n = 0;

	if (!fp) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), fp) && n < max) {
		char name[64], addr[160];

		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0])
			continue;
		if (sscanf(line, "%63s %159s", name, addr) != 2)
			continue;
		snprintf(peers[n].addr, sizeof(peers[n].addr), "%s", addr);
		peers[n].nid = 0;
		n++;
	}
	fclose(fp);
	return n;
}

/* "krgtool spawn --auto [--nodes FILE] cmd": kernel picks, we forward. */
static int spawn_auto(const char *nodes_path, const char *command)
{
	static struct peer_nid peers[KRGV_NODES];
	struct krgv_place p;
	char cmd[1200];
	int n, i, rc = -1;

	n = load_node_list(nodes_path, peers, KRGV_NODES);
	if (n <= 0) {
		fprintf(stderr, "no nodes in %s\n", nodes_path);
		return -1;
	}

	for (i = 0; i < n; i++) {
		unsigned nid = 0, run = 0;

		if (collect_one(peers[i].addr, &nid, &run) == 0) {
			peers[i].nid = nid;
			fprintf(stderr, "  %-12s node %-3u runq %u\n",
				peers[i].addr, nid, run);
		} else {
			peers[i].nid = 0;
		}
	}

	if (kernel_place(&p) < 0)
		return -1;

	if (p.node == p.self) {
		snprintf(cmd, sizeof(cmd), "SPAWN %s\n", command);
		rc = local_write_cmd(cmd, strlen(cmd)) ? -1 : 0;
	} else {
		for (i = 0; i < n; i++) {
			if (peers[i].nid != p.node)
				continue;
			snprintf(cmd, sizeof(cmd), "SPAWN %s\n", command);
			rc = remote_cmd(peers[i].addr, cmd);
			break;
		}
		if (i == n) {
			fprintf(stderr, "node %u is not in %s\n", p.node, nodes_path);
			rc = -1;
		}
	}

	if (rc)
		kernel_release(p.ticket);
	fprintf(stderr, "placed node=%u ticket=%u runq=%u self=%u %s\n",
		p.node, p.ticket, p.runq, p.self, rc ? "FAILED" : "ok");
	return rc;
}

static void serve(int port)
{
	int ls, one = 1;

	ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) {
		perror("socket");
		exit(1);
	}
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);

	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		exit(1);
	}
	listen(ls, 16);
	fprintf(stderr, "krgtool serve: listening on %d\n", port);

	for (;;) {
		int fd = accept(ls, NULL, NULL);
		char req[1024];
		struct krgv_task *t;
		unsigned n, i;
		ssize_t r;

		if (fd < 0)
			continue;

		r = read(fd, req, sizeof(req) - 1);
		if (r <= 0) {
			close(fd);
			continue;
		}
		req[r] = 0;
		req[strcspn(req, "\r\n")] = 0;

		if (!strncmp(req, "PING", 4)) {
			write(fd, "PONG\n", 5);
		} else if (!strncmp(req, "LIST", 4)) {
			if (read_local(&t, &n) == 0) {
				unsigned nid = 0, cnt = 0, run = 0;
				char sum[96];

				if (local_load(&nid, &cnt, &run) == 0) {
					int l = snprintf(sum, sizeof(sum),
							 "# load %u %u\n", run, cnt);
					write(fd, sum, l);
				}
				{
					unsigned mt = 0, mf = 0;

					if (local_mem(&nid, &mt, &mf) == 0) {
						int l = snprintf(sum, sizeof(sum),
								 "# mem %u %u\n", mt, mf);
						write(fd, sum, l);
					}
				}
				for (i = 0; i < n; i++) {
					char line[160];
					int len = snprintf(line, sizeof(line),
						"%u %u %s %u %u %u %s\n",
						t[i].pid, t[i].ppid,
						st_name(t[i].state), t[i].tgid,
						t[i].rss_kb, t[i].gpid, t[i].comm);
					if (write(fd, line, len) != len)
						break;
				}
				free_rec(t);
			}
			write(fd, "END\n", 4);
		} else if (!strncmp(req, "LOAD", 4)) {
			unsigned nid = 0, cnt = 0, run = 0;
			char rep[96];
			int len;

			if (local_load(&nid, &cnt, &run) == 0)
				len = snprintf(rep, sizeof(rep), "OK %u %u %u\n",
					       nid, run, cnt);
			else
				len = snprintf(rep, sizeof(rep), "ERR\n");
			write(fd, rep, len);
		} else if (!strncmp(req, "MEM", 3)) {
			unsigned nid = 0, mt = 0, mf = 0;
			char rep[96];
			int len;

			if (local_mem(&nid, &mt, &mf) == 0)
				len = snprintf(rep, sizeof(rep), "OK %u %u %u\n",
					       nid, mt, mf);
			else
				len = snprintf(rep, sizeof(rep), "ERR\n");
			write(fd, rep, len);
		} else if (!strncmp(req, "EXEC ", 5)) {
			FILE *pp = popen(req + 5, "r");
			char buf[1024];

			if (!pp) {
				write(fd, "ERR exec\n", 9);
			} else {
				while (fgets(buf, sizeof(buf), pp))
					write(fd, buf, strlen(buf));
				pclose(pp);
				write(fd, "END\n", 4);
			}
		} else if (!strncmp(req, "SPAWN ", 6) ||
			   !strncmp(req, "KILL ", 5)) {
			int err = local_write_cmd(req, strlen(req));
			char rep[64];
			int len;

			if (err)
				len = snprintf(rep, sizeof(rep), "ERR %d\n", errno);
			else
				len = snprintf(rep, sizeof(rep), "OK\n");
			write(fd, rep, len);
		} else {
			write(fd, "ERR unknown\n", 12);
		}
		close(fd);
	}
}

static void usage(void)
{
	fprintf(stderr,
		"usage: krgtool local\n"
		"       krgtool serve [port]\n"
		"       krgtool show peer:port[,peer:port...]\n"
		"       krgtool spawn peer:port \"command\"\n"
		"       krgtool spawn --auto [--nodes FILE] \"command\"\n"
		"       krgtool kill peer:port gpid\n"
		"       krgtool load peer:port\n"
		"       krgtool mem peer:port\n"
		"       krgtool exec peer:port \"command\"\n"
		"       krgtool place\n"
		"       krgtool release ticket\n"
		"       krgtool state\n");
}

static int print_state(void)
{
	struct krgv_state *s;
	int fd, i;

	fd = open(DEV, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", DEV, strerror(errno));
		return -1;
	}
	s = malloc(sizeof(*s));
	if (!s) {
		close(fd);
		return -1;
	}
	if (ioctl(fd, KRGV_IOC_STATE, s) < 0) {
		fprintf(stderr, "STATE: %s\n", strerror(errno));
		close(fd);
		free(s);
		return -1;
	}
	close(fd);

	printf("self %u  nodes with load %u  reservations open %u\n",
	       s->self, s->nvalid, s->tickets_used);
	printf("%-6s %-8s %-10s %s\n", "NODE", "RUNQ", "RESERVED", "SEQ");
	for (i = 0; i < KRGV_NODES; i++) {
		if (!s->n[i].valid)
			continue;
		printf("%-6d %-8u %-10u %u\n", i, s->n[i].runq,
		       s->n[i].reserved, s->n[i].seq);
	}
	free(s);
	return 0;
}

int main(int argc, char **argv)
{
	char node[128], cmd[1024];

	if (argc < 2) {
		usage();
		return 1;
	}
	gethostname(node, sizeof(node));

	if (!strcmp(argv[1], "local")) {
		printf("%-10s %8s %6s %6s %-2s %8s %8s  %s\n",
		       "NODE", "GPID", "PID", "PPID", "ST", "TGID", "RSS_KB", "COMM");
		return show_local(node) ? 1 : 0;
	}

	if (!strcmp(argv[1], "serve"))
		return serve(argc > 2 ? atoi(argv[2]) : 7700), 0;

	if (!strcmp(argv[1], "show")) {
		char *peers, *tok, *save = NULL;

		printf("%-10s %8s %6s %6s %-2s %8s %8s  %s\n",
		       "NODE", "GPID", "PID", "PPID", "ST", "TGID", "RSS_KB", "COMM");
		show_local(node);
		if (argc > 2) {
			peers = strdup(argv[2]);
			for (tok = strtok_r(peers, ",", &save); tok;
			     tok = strtok_r(NULL, ",", &save))
				show_peer(tok);
			free(peers);
		}
		return 0;
	}

	if (!strcmp(argv[1], "spawn") && argc >= 4 && !strcmp(argv[2], "--auto")) {
		const char *nodes = getenv("KRG_NODES");
		char command[1024];
		int i = 3;

		if (!nodes)
			nodes = "/etc/krg-nodes.list";
		if (!strcmp(argv[3], "--nodes")) {
			if (argc < 6) {
				usage();
				return 1;
			}
			nodes = argv[4];
			i = 5;
		}
		if (i >= argc) {
			usage();
			return 1;
		}
		snprintf(command, sizeof(command), "%s", argv[i]);
		return spawn_auto(nodes, command) ? 1 : 0;
	}

	if (!strcmp(argv[1], "spawn") && argc >= 4) {
		snprintf(cmd, sizeof(cmd), "SPAWN %s\n", argv[3]);
		return remote_cmd(argv[2], cmd) ? 1 : 0;
	}

	if (!strcmp(argv[1], "place")) {
		struct krgv_place p;

		if (kernel_place(&p))
			return 1;
		printf("node %u ticket %u runq %u self %u\n",
		       p.node, p.ticket, p.runq, p.self);
		return 0;
	}

	if (!strcmp(argv[1], "release") && argc >= 3) {
		kernel_release((unsigned)strtoul(argv[2], NULL, 10));
		return 0;
	}

	if (!strcmp(argv[1], "state"))
		return print_state() ? 1 : 0;

	if (!strcmp(argv[1], "kill") && argc >= 4) {
		snprintf(cmd, sizeof(cmd), "KILL %s\n", argv[3]);
		return remote_cmd(argv[2], cmd) ? 1 : 0;
	}

	if (!strcmp(argv[1], "load") && argc >= 3)
		return remote_cmd(argv[2], "LOAD\n") ? 1 : 0;

	if (!strcmp(argv[1], "mem") && argc >= 3)
		return remote_cmd(argv[2], "MEM\n") ? 1 : 0;

	if (!strcmp(argv[1], "exec") && argc >= 4) {
		snprintf(cmd, sizeof(cmd), "EXEC %s\n", argv[3]);
		return remote_exec(argv[2], cmd) ? 1 : 0;
	}

	usage();
	return 1;
}
