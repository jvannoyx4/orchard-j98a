// SPDX-License-Identifier: MIT
/* Hold a normally initialized H4 adapter; never inquire, pair, or print its MAC. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#define N_HCI 15
#define BTPROTO_HCI 1
#define HCI_CHANNEL_CONTROL 3
#define MGMT_READ_INFO 4
#define MGMT_SET_POWERED 5
#define UNSAFE_DISCOVERY_SETTINGS ((1U << 1) | (1U << 3) | (1U << 10))
struct sockaddr_hci_local { sa_family_t family; uint16_t device, channel; };
static volatile sig_atomic_t stopping;
static void interrupted(int sig) { (void)sig; stopping = 1; }
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p+2) << 16; }
static int64_t now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int wait_for(int fd, short events, int64_t deadline)
{
	struct pollfd p = { .fd=fd, .events=events };
	for (;;) {
		int64_t left = deadline-now_ms();
		int n;
		if (stopping) { errno=EINTR; return -1; }
		if (left <= 0) { errno=ETIMEDOUT; return -1; }
		n = poll(&p, 1, (int)left);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0) return -1;
		if (!n) { errno=ETIMEDOUT; return -1; }
		if (p.revents & (POLLERR|POLLHUP|POLLNVAL)) { errno=EIO; return -1; }
		if (p.revents & events) return 0;
	}
}

static ssize_t send_management(int fd, const uint8_t *packet, size_t length)
{
	ssize_t n=send(fd,packet,length,MSG_NOSIGNAL);
#ifdef HCI_MANAGED_TEST
	/* A Unix socket normally returns length. Model the management socket's
	 * early CommandStatus return separately, without changing its packet. */
	static int first=1;
	const char *override=getenv("HCI_MANAGED_TEST_FIRST_SEND_RESULT");
	if (first && n == (ssize_t)length && override) n=strtol(override,NULL,10);
	first=0;
#endif
	return n;
}

/* Return an actual management status, or -1 for transport/protocol failure. */
static int mgmt_command(int fd, unsigned int index, unsigned int opcode,
			int param, uint8_t *reply, size_t expected, int64_t deadline)
{
	uint8_t tx[7]={opcode & 255,opcode >> 8,index & 255,index >> 8,0,0,0}, packet[4096];
	unsigned int packets;
	size_t txlen = param < 0 ? 6 : 7;
	ssize_t n;
	if (param >= 0) { tx[4]=1; tx[6]=param; }
	if (wait_for(fd,POLLOUT,deadline)) return -1;
	n=send_management(fd,tx,txlen);
	/* hci_mgmt_cmd returns mgmt_cmd_status's zero success result for early
	 * rejection (notably InvalidIndex during HCI_SETUP). That status is
	 * queued on the socket; receive it instead of aborting initialization.
	 * Datagram writes are never retried as a partial byte stream. */
	if (n < 0 || (n != 0 && n != (ssize_t)txlen)) {
		if (n >= 0) errno=EIO;
		return -1;
	}
	for (packets=0; packets<128; packets++) {
		unsigned int event, length;
		if (wait_for(fd,POLLIN,deadline)) return -1;
		n=recv(fd,packet,sizeof(packet),MSG_TRUNC);
		if (n < 0) return -1;
		if (n < 6 || n > (ssize_t)sizeof(packet)) { errno=EPROTO; return -1; }
		event=le16(packet); length=le16(packet+4);
		if (n != length+6) { errno=EPROTO; return -1; }
		if (le16(packet+2) != index) continue;
		if (event == 3 || event == 5) { errno=ENODEV; return -1; }
		if (event != 1 && event != 2) continue;
		if (length < 3 || (event == 2 && length != 3)) { errno=EPROTO; return -1; }
		if (le16(packet+6) != opcode) continue;
		if (packet[8]) return packet[8];
		if (event != 1 || length != expected+3) { errno=EPROTO; return -1; }
		memcpy(reply,packet+9,expected);
		return 0;
	}
	errno=EOVERFLOW; return -1;
}

static int load_factory(const char *path, uint8_t factory[6])
{
	FILE *f=fopen(path,"rb");
	int bad;
	if (!f) { perror("open private factory-address file"); return -1; }
	bad=fread(factory,1,6,f) != 6 || fgetc(f) != EOF || ferror(f);
	fclose(f);
	if (bad) { fprintf(stderr,"Factory-address file must contain exactly six display-order bytes.\n"); return -1; }
	return 0;
}

static int address_matches(const uint8_t address[6], const uint8_t factory[6])
{
	unsigned int i;
	uint8_t any=0, all=255;
	for (i=0; i<6; i++) { any |= factory[i]; all &= factory[i]; }
	if (!any || all == 255) return 0;
	/* Management's bdaddr_t is little-endian; ADT factory data is display order. */
	for (i=0; i<6; i++) if (address[i] != factory[5-i]) return 0;
	return 1;
}

static int managed_probe(int sock, unsigned int index, const uint8_t factory[6],
			 int64_t deadline, int hold)
{
	uint8_t info[280], settings[4];
	int rc=1, status, powering_attempted=0;
	struct timespec pause={.tv_nsec=100000000};
	int64_t init_deadline=now_ms()+30000;
	if (deadline < init_deadline) init_deadline=deadline;
	for (;;) {
		status=mgmt_command(sock,index,MGMT_READ_INFO,-1,info,sizeof(info),init_deadline);
		if (status != 0x11) break; /* Not visible until normal kernel setup completes. */
		if (now_ms() >= init_deadline || stopping || nanosleep(&pause,NULL)) break;
	}
	if (status) {
		fprintf(stderr,"Managed ReadInfo failed (status %d, errno %d).\n",status,status < 0 ? errno : 0);
		goto out;
	}
	printf("{\"hci_index\":%u,\"hci_version\":%u,\"manufacturer\":%u,\"factory_address_match\":%s,\"current_settings\":%u}\n",
	       index,info[6],le16(info+7),address_matches(info,factory) ? "true" : "false",le32(info+13));
	if (info[6] != 8 || le16(info+7) != 15 || !address_matches(info,factory)) {
		fprintf(stderr,"Managed identity/address differs from measured patched J98a; no address was changed.\n");
		goto out;
	}
	if (le32(info+13) & UNSAFE_DISCOVERY_SETTINGS) {
		fprintf(stderr,"Unexpected connectable/discoverable/advertising settings; stopping.\n"); goto out;
	}
	powering_attempted=1;
	status=mgmt_command(sock,index,MGMT_SET_POWERED,1,settings,sizeof(settings),init_deadline);
	if (status || !(le32(settings)&1) || (le32(settings)&UNSAFE_DISCOVERY_SETTINGS)) {
		fprintf(stderr,"Managed power-on failed or enabled unexpected settings (status %d).\n",status); goto out;
	}
	printf("{\"managed_ready\":true,\"hci_index\":%u,\"powered\":true,\"factory_address_match\":true,\"current_settings\":%u}\n",index,le32(settings));
	rc=0;
	while (hold && !stopping && now_ms() < deadline) {
		struct pollfd p={.fd=sock,.events=POLLIN};
		uint8_t event[4096];
		int n=poll(&p,1,250);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) { rc=1; break; }
		if (n > 0) {
			ssize_t size=recv(sock,event,sizeof(event),MSG_TRUNC);
			if (size < 6 || size > (ssize_t)sizeof(event) || size != le16(event+4)+6) { rc=1; break; }
			if (le16(event+2) == index && (le16(event) == 3 || le16(event) == 5)) { rc=1; break; }
		}
	}
out:
	/* This newly created adapter is ours; try to power it down before detach. */
	if (powering_attempted || (!status && (le32(info+13)&1))) {
		stopping=0;
		status=mgmt_command(sock,index,MGMT_SET_POWERED,0,settings,sizeof(settings),now_ms()+3000);
		if (status || (le32(settings)&1)) { fprintf(stderr,"Managed power-down not acknowledged; tty detach follows.\n"); rc=1; }
	}
	return rc;
}

#ifndef HCI_MANAGED_TEST
int main(int argc, char **argv)
{
	struct termios saved, raw;
	struct sigaction sa={.sa_handler=interrupted};
	struct sockaddr_hci_local address={.family=AF_BLUETOOTH,.device=0xffff,.channel=HCI_CHANNEL_CONTROL};
	uint8_t factory[6];
	int tty=-1,sock=-1,id,original=0,discipline=N_HCI,exclusive=0,have_saved=0,attached=0,rc=1;
	int64_t deadline=now_ms()+120000;
	if (argc != 3) { fprintf(stderr,"Usage: %s /dev/ttySAC3 /private/factory-address.bin\n",argv[0]); return 2; }
	if (load_factory(argv[2],factory)) return 1;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); sigaction(SIGHUP,&sa,NULL);
	setvbuf(stdout,NULL,_IONBF,0);
	tty=open(argv[1],O_RDWR|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);
	if (tty < 0) { perror("open tty"); goto out; }
	if (ioctl(tty,TIOCGETD,&original) || original != 0) { fprintf(stderr,"Expected N_TTY.\n"); goto out; }
	if (ioctl(tty,TIOCEXCL)) { perror("exclusive tty"); goto out; }
	exclusive=1;
	if (tcgetattr(tty,&saved)) { perror("read termios"); goto out; }
	have_saved=1; raw=saved; cfmakeraw(&raw);
	raw.c_cflag &= ~(CSIZE|PARENB|CSTOPB|HUPCL);
	raw.c_cflag |= CS8|CLOCAL|CREAD|CRTSCTS;
	raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
	if (cfsetispeed(&raw,B115200) || cfsetospeed(&raw,B115200) || tcsetattr(tty,TCSANOW,&raw) || tcflush(tty,TCIOFLUSH)) { perror("configure tty"); goto out; }
	if (tcgetattr(tty,&raw) || cfgetispeed(&raw)!=B115200 || cfgetospeed(&raw)!=B115200 || !(raw.c_cflag&CRTSCTS)) { fprintf(stderr,"TTY configuration readback failed.\n"); goto out; }
	sock=socket(AF_BLUETOOTH,SOCK_RAW|SOCK_NONBLOCK|SOCK_CLOEXEC,BTPROTO_HCI);
	if (sock < 0 || bind(sock,(struct sockaddr *)&address,sizeof(address))) { perror("bind management socket"); goto out; }
	if (ioctl(tty,TIOCSETD,&discipline)) { perror("attach N_HCI"); goto out; }
	attached=1;
	/* RESET_ON_INIT only: normal H4 setup, no RAW_DEVICE/VND_DETECT/BCM setup. */
	if (ioctl(tty,HCIUARTSETFLAGS,2) || ioctl(tty,HCIUARTSETPROTO,0)) { perror("normal H4 registration"); goto out; }
	id=ioctl(tty,HCIUARTGETDEVICE);
	if (id < 0 || id >= UINT16_MAX) { perror("get HCI index"); goto out; }
	printf("{\"hci_index\":%d,\"transport\":\"managed_H4\",\"baud\":115200}\n",id);
	rc=managed_probe(sock,(unsigned int)id,factory,deadline,1);
out:
	if (sock >= 0) close(sock);
	if (attached && ioctl(tty,TIOCSETD,&original)) { perror("restore N_TTY"); rc=1; }
	if (have_saved && tcsetattr(tty,TCSANOW,&saved)) { perror("restore termios"); rc=1; }
	if (exclusive) ioctl(tty,TIOCNXCL);
	if (tty >= 0) close(tty);
	puts("{\"managed_session_finished\":true}");
	return rc;
}
#else
int main(int argc, char **argv)
{
	uint8_t factory[6];
	if ((argc != 3 && argc != 4) || load_factory(argv[2],factory)) return 2;
	signal(SIGTERM,interrupted);
	setvbuf(stdout,NULL,_IONBF,0);
	return managed_probe(atoi(argv[1]),7,factory,now_ms()+5000,argc==4);
}
#endif
