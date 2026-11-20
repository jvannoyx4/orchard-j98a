// SPDX-License-Identifier: MIT
/* Keep one UART open across the separately qualified Patchram/H4 programs. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void interrupted(int sig) { (void)sig; stopping=1; }
static int64_t now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC,&t)) return -1;
	return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static void snapshot(int tty, const char *stage)
{
	struct serial_icounter_struct count;
	int modem;
	printf("{\"stage\":\"%s\"",stage);
	if (!ioctl(tty,TIOCMGET,&modem)) printf(",\"cts_asserted\":%s",modem&TIOCM_CTS ? "true" : "false");
	if (!ioctl(tty,TIOCGICOUNT,&count))
		printf(",\"tx\":%u,\"rx\":%u,\"frame_errors\":%u,\"overruns\":%u,\"parity_errors\":%u,\"breaks\":%u",
		       (unsigned int)count.tx,(unsigned int)count.rx,(unsigned int)count.frame,
		       (unsigned int)count.overrun,(unsigned int)count.parity,(unsigned int)count.brk);
	puts("}");
}

static int run_child(char *const command[], int64_t deadline)
{
	pid_t child=fork();
	int status, terminated=0, killed=0;
	int64_t grace=0;
	if (child < 0) { perror("fork helper"); return 1; }
	if (!child) {
		/* Supervisor forwards termination and owns this process group's cleanup. */
		setpgid(0,0);
		execv(command[0],command);
		perror("exec helper"); _exit(127);
	}
	setpgid(child,child);
	for (;;) {
		pid_t result=waitpid(child,&status,WNOHANG);
		if (result == child) {
			if (terminated) return 143;
			return WIFEXITED(status) ? WEXITSTATUS(status) : 128+WTERMSIG(status);
		}
		if (result < 0 && errno != EINTR) { perror("wait helper"); return 1; }
		if (!terminated && (stopping || now_ms() >= deadline)) {
			kill(-child,SIGTERM);
			terminated=1; grace=now_ms()+8000;
		}
		if (terminated && !killed && now_ms() >= grace) {
			kill(-child,SIGKILL);
			killed=1; grace=now_ms()+2000;
		}
		if (killed && now_ms() >= grace) {
			fprintf(stderr,"Helper could not be reaped; stop device testing and use hardware reset.\n");
			return 125;
		}
		poll(NULL,0,25);
	}
}

static int still_configured(int tty)
{
	struct termios t;
	int discipline;
	if (tcgetattr(tty,&t) || ioctl(tty,TIOCGETD,&discipline)) return 0;
	return discipline == 0 && cfgetispeed(&t)==B115200 && cfgetospeed(&t)==B115200 &&
	       (t.c_cflag&CRTSCTS) && !(t.c_cflag&HUPCL);
}

int main(int argc, char **argv)
{
	struct termios saved, raw;
	struct sigaction sa={.sa_handler=interrupted};
	char *patch_args[4], *managed_args[4], *factory_args[4];
	int tty=-1, saved_valid=0, original=0, rc=1;
	int64_t deadline=now_ms()+120000;
	if (argc != 6 && argc != 7) {
		fprintf(stderr,"Usage: %s tty USI.hcd private-factory.bin patchram-helper managed-helper [factory-helper]\n",argv[0]); return 2;
	}
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); sigaction(SIGHUP,&sa,NULL);
	setvbuf(stdout,NULL,_IONBF,0);
	tty=open(argv[1],O_RDWR|O_NONBLOCK|O_NOCTTY|O_CLOEXEC);
	if (tty < 0) { perror("open UART guard"); goto out; }
	if (ioctl(tty,TIOCGETD,&original) || original != 0) { fprintf(stderr,"UART guard requires N_TTY.\n"); goto out; }
	if (tcgetattr(tty,&saved)) { perror("read original termios"); goto out; }
	saved_valid=1; raw=saved; cfmakeraw(&raw);
	raw.c_cflag &= ~(CSIZE|PARENB|CSTOPB|HUPCL);
	raw.c_cflag |= CS8|CLOCAL|CREAD|CRTSCTS;
	raw.c_cc[VMIN]=0; raw.c_cc[VTIME]=0;
	if (cfsetispeed(&raw,B115200) || cfsetospeed(&raw,B115200) || tcsetattr(tty,TCSANOW,&raw) ||
	    tcflush(tty,TCIOFLUSH) || !still_configured(tty)) { perror("configure UART guard"); goto out; }
	/* This fd stays open but is never read/written; children own all HCI I/O. */
	snapshot(tty,"before_patchram");
	patch_args[0]=argv[4]; patch_args[1]=argv[1]; patch_args[2]=argv[2]; patch_args[3]=NULL;
	rc=run_child(patch_args,deadline);
	snapshot(tty,"after_patchram");
	if (rc || stopping) goto out;
	if (!still_configured(tty)) { fprintf(stderr,"UART state changed across Patchram helper; stopping.\n"); rc=1; goto out; }
	if (argc == 7) {
		factory_args[0]=argv[6]; factory_args[1]=argv[1]; factory_args[2]=argv[3]; factory_args[3]=NULL;
		rc=run_child(factory_args,deadline);
		snapshot(tty,"after_factory_address");
		if (rc || stopping) goto out;
		if (!still_configured(tty)) { fprintf(stderr,"UART state changed across factory helper; stopping.\n"); rc=1; goto out; }
	}
	managed_args[0]=argv[5]; managed_args[1]=argv[1]; managed_args[2]=argv[3]; managed_args[3]=NULL;
	rc=run_child(managed_args,deadline);
	snapshot(tty,"after_managed");
out:
	/* Do not race a child which is stuck in an uninterruptible kernel call. */
	if (saved_valid && rc != 125) {
		if (ioctl(tty,TIOCSETD,&original)) { perror("restore original line discipline"); rc=1; }
		if (tcsetattr(tty,TCSANOW,&saved)) { perror("restore original termios"); rc=1; }
	}
	if (tty >= 0) close(tty);
	puts("{\"uart_supervisor_finished\":true}");
	return rc;
}
