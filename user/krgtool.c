/* krgtool - Kerrighed-mini userspace side.
 *
 *   krgtool local                 print this node's task table
 *   krgtool serve [port]          serve this node + SPAWN/KILL forwarding
 *   krgtool show peer:port,...    merge local + remote nodes into one table
 *   krgtool spawn peer:port cmd   ask that node's kernel to create a task
 *   krgtool kill  peer:port gpid  ask that node's kernel to kill a task
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
#include <sys/types.h>
#include <arpa/inet.h>

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
		"       krgtool kill peer:port gpid\n"
		"       krgtool load peer:port\n"
		"       krgtool mem peer:port\n"
		"       krgtool exec peer:port \"command\"\n");
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

	if (!strcmp(argv[1], "spawn") && argc >= 4) {
		snprintf(cmd, sizeof(cmd), "SPAWN %s\n", argv[3]);
		return remote_cmd(argv[2], cmd) ? 1 : 0;
	}

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
