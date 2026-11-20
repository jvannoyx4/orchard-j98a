// SPDX-License-Identifier: MIT
/* J98a: restore the same device factory address in BCM runtime state only. */
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
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* UAPI used by Linux drivers/bluetooth/hci_ldisc.c and hci_uart.h. */
#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#define HCI_UART_H4 0
#define HCI_RAW_AND_RESET_FLAGS 3 /* RAW_DEVICE | RESET_ON_INIT */
#define N_HCI 15
#define BTPROTO_HCI 1
#define HCI_CHANNEL_USER 1
struct sockaddr_hci_local { sa_family_t family; uint16_t device, channel; };

static volatile sig_atomic_t stopping;
static void interrupted(int sig) { (void)sig; stopping = 1; }

static int64_t now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int ready(int fd, short events, int64_t end)
{
	struct pollfd p = { .fd = fd, .events = events };
	for (;;) {
		int64_t left = end - now_ms();
		int rc;
		if (stopping) { errno = EINTR; return -1; }
		if (left <= 0) { errno = ETIMEDOUT; return -1; }
		rc = poll(&p, 1, (int)left);
		if (rc < 0 && errno == EINTR) continue;
		if (rc < 0) return -1;
		if (!rc) { errno = ETIMEDOUT; return -1; }
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; return -1; }
		if (p.revents & events) return 0;
	}
}

static unsigned int le16(const uint8_t *p) { return p[0] | (unsigned int)p[1] << 8; }

static int64_t total_deadline;

static int command_data(int fd, unsigned int opcode, const uint8_t *params,
			unsigned int param_len, unsigned int reply_len, uint8_t *reply)
{
	uint8_t tx[259] = { 1, opcode & 255, opcode >> 8, param_len }, packet[260];
	int64_t end = now_ms() + 3000;
	unsigned int n;
	ssize_t len;
	if (param_len > 255) { errno = EINVAL; return -1; }
	if (param_len) memcpy(tx + 4, params, param_len);
	if (total_deadline && total_deadline < end) end = total_deadline;
	if (ready(fd, POLLOUT, end)) return -1;
	len = send(fd, tx, param_len + 4, MSG_NOSIGNAL);
	if (len != param_len + 4) { if (len >= 0) errno = EIO; return -1; }
	for (n = 0; n < 16; n++) {
		if (ready(fd, POLLIN, end)) return -1;
		len = recv(fd, packet, sizeof(packet), MSG_TRUNC);
		if (len < 0) return -1;
		if (len < 3 || len > (ssize_t)sizeof(packet) || packet[0] != 4 || len != packet[2] + 3) {
			errno = EPROTO; return -1;
		}
		if (packet[1] == 0x10) { errno = EIO; return -1; }
		if (packet[1] == 0x0f) {
			if (packet[2] != 4) { errno = EPROTO; return -1; }
			if (le16(packet + 5) == opcode && packet[3]) {
				fprintf(stderr, "HCI 0x%04x Command Status 0x%02x\n", opcode, packet[3]);
				errno = EREMOTEIO; return -1;
			}
			continue;
		}
		if (packet[1] != 0x0e) continue;
		if (packet[2] < 4) { errno = EPROTO; return -1; }
		if (le16(packet + 4) != opcode) continue;
		if (packet[2] != reply_len + 3) { errno = EPROTO; return -1; }
		if (packet[6]) {
			fprintf(stderr, "HCI 0x%04x completion status 0x%02x\n", opcode, packet[6]);
			errno = EREMOTEIO; return -1;
		}
		/* Every next command requires controller-granted command credit. */
		if (!packet[3]) { errno = EPROTO; return -1; }
		memcpy(reply, packet + 6, reply_len);
		return 0;
	}
	errno = EOVERFLOW;
	return -1;
}

static int command(int fd, unsigned int opcode, unsigned int reply_len, uint8_t *reply)
{
	return command_data(fd, opcode, NULL, 0, reply_len, reply);
}


static uint8_t factory_hci[6];

/* Input provenance: six display-order bytes read from this physical device's
 * ADT /chosen/mac-address-bluetooth0 and cross-checked with UART3 local address.
 * There is deliberately no literal-address option or generated substitute. */
static int load_factory_address(const char *path)
{
    struct stat st;
    uint8_t display[7], any=0, all=255;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK), bad=0;
    ssize_t count;
    if (fd < 0) { perror("open private same-device factory file"); return 1; }
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size != 6 ||
        st.st_uid != geteuid() || (st.st_mode&077)) {
        fputs("Factory file must be six private bytes in a regular caller-owned file.\n",stderr);
        close(fd); return 1;
    }
    count=read(fd,display,sizeof(display));
    close(fd);
    if (count != 6) bad=1;
    if (!bad) {
        for (unsigned int i=0; i<6; i++) { any|=display[i]; all&=display[i]; }
        if (!any || all == 255 || (display[0]&1)) bad=1;
    }
    if (bad) { fputs("Invalid factory address; no device opened.\n",stderr); return 1; }
    for (unsigned int i=0; i<6; i++) factory_hci[i]=display[5-i];
    return 0;
}

static int provision_factory_address(int fd)
{
    uint8_t version[9], address[7], ack[1];
    int already_matches;
    total_deadline=now_ms()+10000;
    if (command(fd,0x1001,9,version)) { perror("read patched identity"); return 1; }
    if (version[1] != 8 || le16(version+2) != 0x133e || version[4] != 8 ||
        le16(version+5) != 15 || le16(version+7) != 0x6607) {
        fputs("Exact measured USI-patched J98a controller identity required; stopped.\n",stderr);
        return 1;
    }
    if (command(fd,0x1009,7,address)) { perror("read current address privately"); return 1; }
    already_matches=!memcmp(address+1,factory_hci,6);
    printf("{\"patched_identity_match\":true,\"factory_address_already_matches\":%s}\n",
           already_matches ? "true" : "false");
    if (!already_matches) {
        /* Exact runtime command used by Linux btbcm_set_bdaddr(). This does
         * not send a Broadcom NVRAM/flash command or write any storage file. */
        if (command_data(fd,0xfc01,factory_hci,6,1,ack)) {
            perror("runtime factory address command"); return 1;
        }
    }
    if (command(fd,0x1009,7,address)) { perror("verify factory address privately"); return 1; }
    if (memcmp(address+1,factory_hci,6)) {
        fputs("Factory address readback mismatch; no retry and no managed handoff.\n",stderr);
        return 1;
    }
    printf("{\"factory_address_verified\":true,\"runtime_write_performed\":%s}\n",
           already_matches ? "false" : "true");
    return 0;
}

#ifndef HCI_DIAG_TEST
int main(int argc, char **argv)
{
	struct termios saved, raw;
	struct sigaction sa = { .sa_handler = interrupted };
	struct sockaddr_hci_local address = { .family = AF_BLUETOOTH, .channel = HCI_CHANNEL_USER };
	struct timespec pause = { .tv_nsec = 20000000 };
	int tty = -1, sock = -1, discipline = 0, original = 0, rc = 1, id;
	int have_saved = 0, changed_ldisc = 0, exclusive = 0;
	int64_t end;
	if (argc != 3) { fprintf(stderr, "Usage: %s /dev/ttySAC3 /private/same-device-factory.bin\n", argv[0]); return 2; }
	if (load_factory_address(argv[2])) return 1;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);
	tty = open(argv[1], O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (tty < 0) { perror("open tty"); goto out; }
	if (ioctl(tty, TIOCGETD, &original) || original != 0) {
		fprintf(stderr, "TTY must use N_TTY before this probe.\n"); goto out;
	}
	if (ioctl(tty, TIOCEXCL)) { perror("exclusive tty"); goto out; }
	exclusive = 1;
	if (tcgetattr(tty, &saved)) { perror("read termios"); goto out; }
	have_saved = 1; raw = saved;
	cfmakeraw(&raw);
	raw.c_cflag &= ~(CSIZE | PARENB | CSTOPB | HUPCL);
	raw.c_cflag |= CS8 | CLOCAL | CREAD | CRTSCTS;
	raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
	if (cfsetispeed(&raw, B115200) || cfsetospeed(&raw, B115200) ||
	    tcsetattr(tty, TCSANOW, &raw) || tcflush(tty, TCIOFLUSH)) {
		perror("set 115200 8N1 RTS/CTS"); goto out;
	}
	discipline = N_HCI;
	if (ioctl(tty, TIOCSETD, &discipline)) { perror("attach N_HCI (load hci_uart first)"); goto out; }
	changed_ldisc = 1;
	if (ioctl(tty, HCIUARTSETFLAGS, HCI_RAW_AND_RESET_FLAGS) || ioctl(tty, HCIUARTSETPROTO, HCI_UART_H4)) {
		perror("register raw H4 protocol"); goto out;
	}
	id = ioctl(tty, HCIUARTGETDEVICE);
	if (id < 0 || id >= UINT16_MAX) { perror("get HCI index"); goto out; }
	address.device = (uint16_t)id;
	printf("{\"hci_index\":%d,\"transport\":\"kernel_H4_user_channel\",\"baud\":115200}\n", id);
	sock = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_HCI);
	if (sock < 0) { perror("HCI socket"); goto out; }
	end = now_ms() + 3000;
	while (bind(sock, (struct sockaddr *)&address, sizeof(address))) {
		if (errno != EBUSY || stopping || now_ms() >= end) { perror("bind HCI user channel"); goto out; }
		if (nanosleep(&pause, NULL)) goto out;
	}
	rc = provision_factory_address(sock);
out:
	if (sock >= 0) close(sock);
	if (changed_ldisc && ioctl(tty, TIOCSETD, &original)) { perror("restore N_TTY"); rc = 1; }
	if (have_saved && tcsetattr(tty, TCSANOW, &saved)) { perror("restore termios"); rc = 1; }
	if (exclusive) ioctl(tty, TIOCNXCL);
	if (tty >= 0) close(tty);
	return rc;
}
#else
int main(int argc, char **argv)
{
    if (argc != 3 || load_factory_address(argv[2])) return 2;
    signal(SIGTERM,interrupted);
    return provision_factory_address(atoi(argv[1]));
}
#endif
