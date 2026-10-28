/* krgsys - cluster free / top over krgtool serve endpoints.
 *
 * usage: krgsys free  <peer[,peer...]>
 *        krgsys top   <peer[,peer...]>
 * local numbers come from /proc; peers are asked with LIST and we read the
 * "# mem <kb_total> <kb_free>" and "# load <run> <tasks>" lines.
 *
 * 2026-10-28: rows raised to 128 (40-node fleet), peer label is host:port so
 * nodes behind one NAT address stay distinguishable, connect has a 3 s
 * timeout so a dead node cannot stall the whole report.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_ROWS 128
#define CONNECT_TIMEOUT_SEC 3

struct stat_row {
	char name[64];
	unsigned long kb_total, kb_free;
	unsigned run, tasks;
	int ok;
};

static void local_stats(struct stat_row *r)
{
	FILE *f;
	char key[64];
	unsigned long val;
	char unit[16];

	snprintf(r->name, sizeof(r->name), "local");
	r->ok = 1;
	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fscanf(f, "%63s %lu %15s", key, &val, unit) == 3) {
			if (!strcmp(key, "MemTotal:"))
				r->kb_total = val;
			else if (!strcmp(key, "MemFree:"))
				r->kb_free = val;
			if (r->kb_total && r->kb_free)
				break;
		}
		fclose(f);
	}
	f = fopen("/proc/stat", "r");
	if (f) {
		while (fgets(key, sizeof(key), f)) {
			if (sscanf(key, "procs_running %u", &r->run) == 1)
				break;
		}
		fclose(f);
	}
	f = fopen("/proc/loadavg", "r");
	if (f) {
		unsigned n = 0;
		while (fscanf(f, "%63s", key) == 1) {
			n++;
			if (n == 4 && sscanf(key, "%*u/%u", &r->tasks) == 1)
				break;
		}
		fclose(f);
	}
}

static int connect_timeout(struct sockaddr_in *sa)
{
	int fd, rc, err = 0;
	socklen_t elen = sizeof(err);
	fd_set wfds;
	struct timeval tv;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	rc = connect(fd, (struct sockaddr *)sa, sizeof(*sa));
	if (rc < 0 && errno == EINPROGRESS) {
		FD_ZERO(&wfds);
		FD_SET(fd, &wfds);
		tv.tv_sec = CONNECT_TIMEOUT_SEC;
		tv.tv_usec = 0;
		rc = select(fd + 1, NULL, &wfds, NULL, &tv);
		if (rc <= 0) {
			close(fd);
			return -1;
		}
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
			close(fd);
			return -1;
		}
	} else if (rc < 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
	return fd;
}

static int ask(const char *peer, struct stat_row *r)
{
	char host[128], port[16], *colon;
	int fd;
	char buf[4096];
	FILE *fp;

	snprintf(host, sizeof(host), "%s", peer);
	colon = strrchr(host, ':');
	if (!colon)
		return -1;
	*colon = 0;
	snprintf(port, sizeof(port), "%s", colon + 1);

	{
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)atoi(port));
		if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
			return -1;
		fd = connect_timeout(&sa);
		if (fd < 0)
			return -1;
	}

	if (write(fd, "LIST\n", 5) != 5) {
		close(fd);
		return -1;
	}
	fp = fdopen(fd, "r");
	if (!fp) {
		close(fd);
		return -1;
	}
	snprintf(r->name, sizeof(r->name), "%s:%s", host, port);
	r->ok = 1;
	while (fgets(buf, sizeof(buf), fp)) {
		unsigned long t, f;
		unsigned run, tasks;

		if (!strncmp(buf, "END", 3))
			break;
		if (sscanf(buf, "# mem %lu %lu", &t, &f) == 2) {
			r->kb_total = t;
			r->kb_free = f;
			continue;
		}
		if (sscanf(buf, "# load %u %u", &run, &tasks) == 2) {
			r->run = run;
			r->tasks = tasks;
			continue;
		}
		if (buf[0] >= '0' && buf[0] <= '9')
			r->tasks++;
	}
	fclose(fp);
	return 0;
}

int main(int argc, char **argv)
{
	static struct stat_row rows[MAX_ROWS];
	int n = 0, i, bad = 0;
	char *peers, *tok, *save = NULL;
	unsigned long tt = 0, tf = 0;
	unsigned trun = 0, ttasks = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: krgsys free|top <peer[,peer...]>\n");
		return 1;
	}

	local_stats(&rows[n++]);
	if (argc > 2) {
		peers = strdup(argv[2]);
		for (tok = strtok_r(peers, ",", &save); tok && n < MAX_ROWS;
		     tok = strtok_r(NULL, ",", &save)) {
			memset(&rows[n], 0, sizeof(rows[n]));
			if (ask(tok, &rows[n]) == 0)
				n++;
			else
				bad++;
		}
		free(peers);
	}

	if (!strcmp(argv[1], "free")) {
		printf("%-24s %10s %10s %10s %8s\n", "NODE", "TOTAL_MB", "FREE_MB", "USED_MB", "TASKS");
		for (i = 0; i < n; i++) {
			printf("%-24s %10lu %10lu %10lu %8u\n", rows[i].name,
			       rows[i].kb_total / 1024, rows[i].kb_free / 1024,
			       (rows[i].kb_total - rows[i].kb_free) / 1024,
			       rows[i].tasks);
			tt += rows[i].kb_total;
			tf += rows[i].kb_free;
		}
		printf("%-24s %10lu %10lu %10lu  reachable=%d/%d\n", "CLUSTER",
		       tt / 1024, tf / 1024, (tt - tf) / 1024, n - 1, n - 1 + bad);
	} else if (!strcmp(argv[1], "top")) {
		printf("%-24s %6s %8s\n", "NODE", "RUN", "TASKS");
		for (i = 0; i < n; i++) {
			printf("%-24s %6u %8u\n", rows[i].name, rows[i].run, rows[i].tasks);
			trun += rows[i].run;
			ttasks += rows[i].tasks;
		}
		printf("%-24s %6u %8u  reachable=%d/%d\n", "CLUSTER", trun, ttasks,
		       n - 1, n - 1 + bad);
	} else {
		fprintf(stderr, "unknown command %s\n", argv[1]);
		return 1;
	}
	return 0;
}