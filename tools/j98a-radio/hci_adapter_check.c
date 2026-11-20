// SPDX-License-Identifier: MIT
/* Read-only management ReadInfo check; does not attach tty or change settings. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BTPROTO_HCI 1
struct sockaddr_hci_local { sa_family_t family; uint16_t device, channel; };
static unsigned int le16(const uint8_t *p) { return p[0] | (unsigned int)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2)<<16; }
static const char *boolean(int value) { return value ? "true" : "false"; }
static int64_t now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
	return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static int check(int sock, unsigned int index, const uint8_t factory[6])
{
	uint8_t command[6]={4,0,index&255,index>>8,0,0}, packet[4096];
	int64_t deadline=now_ms()+5000;
	unsigned int packets=0;
	ssize_t n;
	n=send(sock,command,sizeof(command),MSG_NOSIGNAL);
	/* A management early CommandStatus can make send return zero. */
	if (n < 0 || (n != 0 && n != sizeof(command))) {
		fputs("ReadInfo send failed.\n",stderr); return 1;
	}
	while (now_ms() < deadline && packets++ < 64) {
		struct pollfd p={.fd=sock,.events=POLLIN};
		unsigned int event,length,i,match=1;
		uint32_t supported,current;
		const uint8_t *info;
		int ok,result=poll(&p,1,100);
		if (result < 0 && errno == EINTR) continue;
		if (result < 0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) break;
		if (!result) { packets--; continue; }
		n=recv(sock,packet,sizeof(packet),MSG_TRUNC);
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (n < 6 || n > (ssize_t)sizeof(packet)) break;
		event=le16(packet); length=le16(packet+4);
		if (n != length+6) break;
		if (le16(packet+2) != index) continue;
		if (event == 3 || event == 5) break;
		if (event != 1 && event != 2) continue;
		if (length < 3 || (event == 2 && length != 3)) break;
		if (le16(packet+6) != 4) continue;
		if (packet[8]) {
			fprintf(stderr,"ReadInfo rejected (management status %u).\n",packet[8]);
			return 1;
		}
		if (event != 1 || length != 283) break;
		info=packet+9;
		for (i=0; i<6; i++) match &= info[i] == factory[5-i];
		supported=le32(info+9); current=le32(info+13);
		ok=info[6] == 8 && le16(info+7) == 15 && match && (current&1) &&
		   (supported&(1U<<7)) && (supported&(1U<<9)) &&
		   !(current&((1U<<1)|(1U<<3)|(1U<<10)));
		printf("{\"adapter_check_ok\":%s,\"hci_index\":%u,\"hci_version\":%u,\"manufacturer\":%u,\"factory_address_match\":%s,\"powered\":%s,\"bredr_supported\":%s,\"le_supported\":%s,\"bredr_enabled\":%s,\"le_enabled\":%s,\"secure_connections_supported\":%s,\"connectable\":%s,\"discoverable\":%s,\"advertising\":%s}\n",
		       boolean(ok),index,info[6],le16(info+7),boolean(match),boolean(current&1),
		       boolean(supported&(1U<<7)),boolean(supported&(1U<<9)),
		       boolean(current&(1U<<7)),boolean(current&(1U<<9)),
		       boolean(supported&(1U<<11)),boolean(current&(1U<<1)),
		       boolean(current&(1U<<3)),boolean(current&(1U<<10)));
		return ok ? 0 : 1;
	}
	fputs("ReadInfo incomplete, malformed, or controller disappeared.\n",stderr);
	return 1;
}

int main(int argc, char **argv)
{
	uint8_t factory[6]={0},any=0,all=255;
	char *end;
	unsigned long index;
	unsigned int i;
	int sock,rc;
	FILE *file;
#ifdef HCI_CHECK_TEST
	if (argc != 4) return 2;
	sock=atoi(argv[3]);
#else
	struct sockaddr_hci_local address={.family=AF_BLUETOOTH,.device=0xffff,.channel=3};
	if (argc != 3) {
		fputs("Usage: hci-adapter-check-aarch64 INDEX /private/factory-address.bin\n",stderr);
		return 2;
	}
#endif
	errno=0; index=strtoul(argv[1],&end,10);
	if (errno || !*argv[1] || *end || index >= 0xffff) return 2;
	file=fopen(argv[2],"rb");
	if (!file) { perror("open private factory-address file"); return 1; }
	rc=fread(factory,1,6,file) != 6 || fgetc(file) != EOF || ferror(file);
	fclose(file);
	for (i=0; i<6; i++) { any |= factory[i]; all &= factory[i]; }
	if (rc || !any || all == 255) {
		fputs("Invalid private factory-address file.\n",stderr); return 1;
	}
#ifndef HCI_CHECK_TEST
	sock=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,BTPROTO_HCI);
	if (sock < 0) { perror("open management socket"); return 1; }
	if (bind(sock,(struct sockaddr *)&address,sizeof(address))) {
		perror("bind management socket"); close(sock); return 1;
	}
#endif
	rc=check(sock,(unsigned int)index,factory);
	close(sock);
	return rc;
}
