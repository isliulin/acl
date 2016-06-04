#include "lib_acl.h"
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include "lib_fiber.h"

static  int  __nfibers = 0;

static void poll_sleep(FIBER *fiber, void *ctx acl_unused)
{
	int  in = 0, fd = dup(in), n;
	struct pollfd pfd;

	memset(&pfd, 0, sizeof(pfd));
	acl_non_blocking(fd, ACL_NON_BLOCKING);
	pfd.fd = fd;
	pfd.events = POLLIN;

	while (1) {
		n = poll(&pfd, 1, 1000);
		if (n < 0)
			break;

		if (n == 0)
			printf("fiber-%d: poll wakeup\r\n", fiber_id(fiber));
		else
			printf("fiber-%d: fd = %d read ready %s\r\n",
				fiber_id(fiber), pfd.fd, pfd.revents & POLLIN
				? "yes" : "no");

		if (pfd.revents & POLLIN) {
			char buf[256];

			n = read(fd, buf, sizeof(buf) - 1);
			if (n < 0) {
				if (errno != EWOULDBLOCK)
					break;
				printf("fiber-%d: %s\r\n", fiber_id(fiber),
					acl_last_serror());
				continue;
			} else if (n == 0)
				break;

			buf[n] = 0;
			printf("fiber-%d: %s", fiber_id(fiber), buf);
			fflush(stdout);
			pfd.revents = 0;
		}
	}

	printf(">>>fiber-%d exit\r\n", fiber_id(fiber));
	if (--__nfibers == 0)
		fiber_io_stop();
}

static void usage(const char *procname)
{
	printf("usage: %s -h [help] -a cmd\r\n", procname);
}

int main(int argc, char *argv[])
{
	int   ch, n = 1;
	char  cmd[128];

	snprintf(cmd, sizeof(cmd), "sleep");

	while ((ch = getopt(argc, argv, "ha:")) > 0) {
		switch (ch) {
		case 'h':
			usage(argv[0]);
			return 0;
		case 'a':
			snprintf(cmd, sizeof(cmd), "%s", optarg);
			break;
		default:
			break;
		}
	}

	__nfibers++;
	fiber_create(poll_sleep, &n, 32768);

	__nfibers++;
	fiber_create(poll_sleep, &n, 32768);

	__nfibers++;
	fiber_create(poll_sleep, &n, 32768);

	__nfibers++;
	fiber_create(poll_sleep, &n, 32768);

	fiber_schedule();

	return 0;
}