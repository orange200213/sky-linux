/* qrtrls - list QRTR name-service servers (debug helper for sky-linux M8)
 *
 * Builds a NEW_LOOKUP control message and sends it to the local name service
 * (node 1, port QRTR_PORT_CTRL). With service==0 the name service matches
 * everything, so this dumps the whole server table: which node registered
 * which service/instance.  Used to find out whether the WCN6750 WPSS firmware
 * has registered its WLFW QMI service (service 0x45).
 *
 *   qrtrls [service] [instance]
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QRTR_PORT_CTRL 0xfffffffeu
#define QRTR_NODE_CTRL 1u

enum { QRTR_TYPE_NEW_SERVER = 4, QRTR_TYPE_NEW_LOOKUP = 10 };

struct sockaddr_qrtr {
	unsigned short sq_family;
	unsigned int sq_node;
	unsigned int sq_port;
};

struct ctrl_pkt {
	unsigned int cmd;
	unsigned int service;
	unsigned int instance;
	unsigned int node;
	unsigned int port;
};

int main(int argc, char **argv)
{
	unsigned int service = argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
	unsigned int instance = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
	struct sockaddr_qrtr addr, ns = { AF_QIPCRTR, QRTR_NODE_CTRL, QRTR_PORT_CTRL };
	struct ctrl_pkt pkt = { QRTR_TYPE_NEW_LOOKUP, service, instance, 0, 0 };
	struct timeval tv = { 2, 0 };
	socklen_t alen = sizeof(addr);
	int fd, n;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	/* the socket already knows the local node id; bind an auto port on it */
	addr.sq_family = AF_QIPCRTR;
	addr.sq_node = 0;
	addr.sq_port = 0;
	if (getsockname(fd, (struct sockaddr *)&addr, &alen) < 0) { perror("getsockname"); return 1; }
	addr.sq_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
	if (getsockname(fd, (struct sockaddr *)&addr, &alen) < 0) { perror("getsockname"); return 1; }
	printf("local %u:%u, lookup service=0x%x instance=%u\n",
	       addr.sq_node, addr.sq_port, service, instance);

	if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ns, sizeof(ns)) < 0) {
		perror("sendto"); return 1;
	}

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	printf("%-8s %-8s %-8s %-8s %s\n", "service", "instance", "node", "port", "name");
	for (;;) {
		struct ctrl_pkt r;
		ssize_t len = recv(fd, &r, sizeof(r), 0);
		if (len < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				printf("(end)\n");
			else
				perror("recv");
			break;
		}
		if (len != sizeof(r)) {
			printf("short packet %zd\n", len);
			continue;
		}
		if (!r.service && !r.instance && !r.node && !r.port && !r.cmd) {
			printf("(end)\n");
			break;
		}
		printf("0x%-6x %-8u %-8u 0x%-6x%s\n", r.service, r.instance, r.node, r.port,
		       r.service == 0x45 ? "  <-- WLFW" : "");
	}
	close(fd);
	(void)n;
	return 0;
}
