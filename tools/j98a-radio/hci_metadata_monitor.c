// SPDX-License-Identifier: MIT
/* Passive HCI monitor: print framing/opcodes/status, never payloads or names. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BTPROTO_HCI 1
#define HCI_CHANNEL_MONITOR 2
struct sockaddr_hci_local { sa_family_t family; uint16_t device, channel; };
static volatile sig_atomic_t stopping;
static void interrupted(int sig) { (void)sig; stopping=1; }
static unsigned int le16(const uint8_t *p) { return p[0] | (unsigned int)p[1]<<8; }
static int64_t now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
	return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static int describe(const uint8_t *p, size_t n, int64_t elapsed)
{
	unsigned int kind,index,length,opcode;
	if (n < 6) return -1;
	kind=le16(p); index=le16(p+2); length=le16(p+4);
	if (n != length+6U) return -1;
	p+=6;
	if (kind == 2) {
		if (length < 3 || length != p[2]+3U) return -1;
		opcode=le16(p);
		printf("{\"ms\":%lld,\"hci_index\":%u,\"kind\":\"command\",\"opcode\":%u,\"parameter_bytes\":%u}\n",
		       (long long)elapsed,index,opcode,p[2]);
	} else if (kind == 3) {
		if (length < 2 || length != p[1]+2U) return -1;
		if (p[0] == 0x0e) {
			if (length < 5) return -1;
			opcode=le16(p+3);
			/* Only these two proven commands expose a status byte here.
			 * Other return parameters might contain identity or vendor data. */
			if ((opcode == 0x0c03 || opcode == 0x1001) && length < 6) return -1;
			printf("{\"ms\":%lld,\"hci_index\":%u,\"kind\":\"command_complete\",\"opcode\":%u,\"command_credit\":%u,\"return_bytes\":%u",
			       (long long)elapsed,index,opcode,p[2],length-5);
			if (opcode == 0x0c03 || opcode == 0x1001) printf(",\"status\":%u",p[5]);
			puts("}");
		} else if (p[0] == 0x0f) {
			if (length != 6) return -1;
			printf("{\"ms\":%lld,\"hci_index\":%u,\"kind\":\"command_status\",\"opcode\":%u,\"command_credit\":%u,\"status\":%u}\n",
			       (long long)elapsed,index,le16(p+4),p[3],p[2]);
		} else {
			printf("{\"ms\":%lld,\"hci_index\":%u,\"kind\":\"event\",\"event\":%u,\"parameter_bytes\":%u}\n",
			       (long long)elapsed,index,p[0],p[1]);
		}
	} else if (kind == 0 || kind == 1 || kind == 8 || kind == 9 || kind == 10) {
		/* NEW_INDEX and INDEX_INFO contain addresses: only their tag is logged. */
		printf("{\"ms\":%lld,\"hci_index\":%u,\"kind\":\"index_event\",\"monitor_opcode\":%u}\n",
		       (long long)elapsed,index,kind);
	}
	/* Silently discard monitor notes, control, ACL/SCO/ISO, vendor diagnostics. */
	return 0;
}

static int monitor(int sock, unsigned int milliseconds)
{
	uint8_t packet[65542];
	unsigned int count=0;
	int64_t start=now_ms();
	int rc=0;
	puts("{\"hci_metadata_monitor_ready\":true,\"maximum_seconds\":120}");
	while (!stopping && now_ms()-start < milliseconds) {
		struct pollfd p={.fd=sock,.events=POLLIN};
		ssize_t bytes;
		int result=poll(&p,1,100);
		if (result < 0 && errno == EINTR) continue;
		if (result < 0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) { rc=1; break; }
		if (!result) continue;
		bytes=recv(sock,packet,sizeof(packet),MSG_TRUNC);
		if (bytes < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (bytes < 0 || bytes > (ssize_t)sizeof(packet) ||
		    describe(packet,(size_t)bytes,now_ms()-start)) {
			fputs("Malformed or truncated monitor datagram; payload omitted.\n",stderr);
			rc=1; break;
		}
		if (++count >= 4096) { fputs("Monitor packet limit reached.\n",stderr); rc=1; break; }
	}
	printf("{\"hci_metadata_monitor_finished\":true,\"datagrams\":%u,\"success\":%s}\n",count,rc ? "false" : "true");
	return rc;
}

int main(int argc, char **argv)
{
	struct sigaction sa={.sa_handler=interrupted};
	int sock,rc;
	setvbuf(stdout,NULL,_IONBF,0);
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM,&sa,NULL); sigaction(SIGINT,&sa,NULL); sigaction(SIGHUP,&sa,NULL);
#ifdef HCI_MONITOR_TEST
	if (argc != 2) return 2;
	sock=atoi(argv[1]);
#else
	struct sockaddr_hci_local address={.family=AF_BLUETOOTH,.device=0xffff,.channel=HCI_CHANNEL_MONITOR};
	int buffer=262144;
	(void)argv;
	if (argc != 1) { fputs("Usage: hci-metadata-monitor-aarch64\n",stderr); return 2; }
	sock=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,BTPROTO_HCI);
	if (sock < 0) { perror("open HCI monitor"); return 1; }
	if (setsockopt(sock,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer)) ||
	    bind(sock,(struct sockaddr *)&address,sizeof(address))) {
		perror("bind HCI monitor"); close(sock); return 1;
	}
#endif
	rc=monitor(sock,120000);
	close(sock);
	return rc;
}
