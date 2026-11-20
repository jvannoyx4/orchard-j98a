// SPDX-License-Identifier: MIT
/* Two-command UART transport diagnostic. No firmware or GPIO operations. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return -1;
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int ready(int fd, short events, int64_t deadline)
{
	struct pollfd p = { .fd = fd, .events = events };
	int64_t now;
	int rc;

	for (;;) {
		now = now_ms();
		if (now < 0)
			return -1;
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		rc = poll(&p, 1, (int)(deadline - now));
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -1;
		if (!rc) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (p.revents & (POLLNVAL | POLLERR | POLLHUP)) {
			errno = EIO;
			return -1;
		}
		if (p.revents & events)
			return 0;
	}
}

static int transfer(int fd, uint8_t *buffer, size_t count,
		    int writing, int64_t deadline)
{
	ssize_t n;

	while (count) {
		if (ready(fd, writing ? POLLOUT : POLLIN, deadline))
			return -1;
		n = writing ? write(fd, buffer, count) : read(fd, buffer, count);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n < 0)
			return -1;
		if (!n)
			continue;
		buffer += n;
		count -= (size_t)n;
	}
	return 0;
}

static unsigned int le16(const uint8_t *p)
{
	return p[0] | (unsigned int)p[1] << 8;
}

static int command(int fd, unsigned int opcode, unsigned int expected_len,
		   uint8_t *reply, unsigned int timeout_ms)
{
	uint8_t tx[] = { 1, opcode & 0xff, opcode >> 8, 0 };
	uint8_t header[3], data[255];
	int64_t started = now_ms();
	unsigned int packets, event, len, completed_opcode;
	int64_t deadline;

	if (started < 0)
		return -1;
	deadline = started + timeout_ms;
	if (transfer(fd, tx, sizeof(tx), 1, deadline))
		return -1;

	/* Bound both elapsed time and unsolicited traffic. Never retry a command. */
	for (packets = 0; packets < 16; packets++) {
		if (transfer(fd, header, 1, 0, deadline))
			return -1;
		if (header[0] != 4) {
			fprintf(stderr, "Unexpected H4 packet type 0x%02x\n", header[0]);
			errno = EPROTO;
			return -1;
		}
		if (transfer(fd, header + 1, 2, 0, deadline))
			return -1;
		event = header[1];
		len = header[2];
		if (transfer(fd, data, len, 0, deadline))
			return -1;
		if (event == 0x10) {
			fprintf(stderr, "Controller reported Hardware Error\n");
			errno = EIO;
			return -1;
		}
		if (event == 0x0f) {
			if (len != 4) {
				errno = EPROTO;
				return -1;
			}
			completed_opcode = le16(data + 2);
			if (completed_opcode == opcode && data[0]) {
				fprintf(stderr, "HCI 0x%04x Command Status 0x%02x\n",
					opcode, data[0]);
				errno = EREMOTEIO;
				return -1;
			}
			continue;
		}
		if (event != 0x0e)
			continue;
		if (len < 4) {
			errno = EPROTO;
			return -1;
		}
		if (le16(data + 1) != opcode)
			continue;
		if (len != expected_len + 3) {
			fprintf(stderr, "HCI 0x%04x reply length %u, expected %u\n",
				opcode, len, expected_len + 3);
			errno = EPROTO;
			return -1;
		}
		if (data[3]) {
			fprintf(stderr, "HCI 0x%04x completion status 0x%02x\n",
				opcode, data[3]);
			errno = EREMOTEIO;
			return -1;
		}
		/* Reset must grant credit before Read Local Version may be sent. */
		if (opcode == 0x0c03 && !data[0]) {
			fprintf(stderr, "Reset returned no command credit; stopping\n");
			errno = EPROTO;
			return -1;
		}
		memcpy(reply, data + 3, expected_len);
		return 0;
	}
	/* Unrelated events are never interpreted or logged as controller identity. */
	errno = EOVERFLOW;
	return -1;
}

int main(int argc, char **argv)
{
	struct termios saved, raw;
	uint8_t reply[9];
	unsigned long timeout_ms = 3000;
	char *end;
	int fd, rc = 1, have_termios = 0, exclusive = 0, discipline, modem;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s /dev/ttySAC3 [timeout-ms-per-command]\n", argv[0]);
		return 2;
	}
	if (argc == 3) {
		errno = 0;
		timeout_ms = strtoul(argv[2], &end, 10);
		if (errno || *end || timeout_ms < 100 || timeout_ms > 10000) {
			fprintf(stderr, "Timeout must be 100..10000 ms\n");
			return 2;
		}
	}
	fd = open(argv[1], O_RDWR | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		perror("open tty");
		return 1;
	}
	if (ioctl(fd, TIOCGETD, &discipline) || discipline != 0) {
		fprintf(stderr, "TTY must use the normal N_TTY line discipline\n");
		goto out;
	}
	if (ioctl(fd, TIOCEXCL)) {
		perror("exclusive tty access");
		goto out;
	}
	exclusive = 1;
	if (tcgetattr(fd, &saved)) {
		perror("read tty settings");
		goto out;
	}
	have_termios = 1;
	raw = saved;
	cfmakeraw(&raw);
	raw.c_cflag &= ~(CSIZE | PARENB | CSTOPB | HUPCL);
	raw.c_cflag |= CS8 | CLOCAL | CREAD | CRTSCTS;
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (cfsetispeed(&raw, B115200) || cfsetospeed(&raw, B115200) ||
	    tcsetattr(fd, TCSANOW, &raw) || tcflush(fd, TCIOFLUSH)) {
		perror("configure 115200 8N1 RTS/CTS");
		goto out;
	}
	if (tcgetattr(fd, &raw) || cfgetispeed(&raw) != B115200 ||
	    cfgetospeed(&raw) != B115200 || !(raw.c_cflag & CRTSCTS)) {
		fprintf(stderr, "TTY did not accept requested baud/flow control\n");
		goto out;
	}
	printf("{\"transport\":\"H4\",\"baud\":115200,\"rtscts\":true}\n");
	if (!ioctl(fd, TIOCMGET, &modem))
		printf("{\"cts_asserted\":%s}\n", modem & TIOCM_CTS ? "true" : "false");
	fflush(stdout);
	if (command(fd, 0x0c03, 1, reply, (unsigned int)timeout_ms)) {
		perror("HCI Reset");
		goto out;
	}
	printf("{\"command\":\"reset\",\"status\":0}\n");
	fflush(stdout);
	if (command(fd, 0x1001, 9, reply, (unsigned int)timeout_ms)) {
		perror("HCI Read Local Version");
		goto out;
	}
	printf("{\"command\":\"read_local_version\",\"status\":0,"
	       "\"hci_version\":%u,\"hci_revision\":%u,\"lmp_version\":%u,"
	       "\"manufacturer\":%u,\"lmp_subversion\":%u}\n",
	       reply[1], le16(reply + 2), reply[4], le16(reply + 5), le16(reply + 7));
	rc = 0;
out:
	/* Discard unsent bytes on failure, and restore the caller's tty settings. */
	if (have_termios) {
		if (rc)
			tcflush(fd, TCOFLUSH);
		if (tcsetattr(fd, TCSANOW, &saved)) {
			perror("restore tty settings");
			rc = 1;
		}
	}
	if (exclusive)
		ioctl(fd, TIOCNXCL);
	close(fd);
	return rc;
}
