#include "lib_acl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fiber/lib_fiber.h"

static unsigned long __max = 10;

static void fiber_producer(FIBER *fiber, void *ctx)
{
	CHANNEL *chan = (CHANNEL *) ctx;
	unsigned long  i;

	for (i = 0; i < __max; i++) {
		int ret = channel_sendul(chan, i);
		if (ret <= 0) {
			printf("fiber-%d, channel_sendul error!\r\n",
				fiber_id(fiber));
			break;
		}

		if (i < 10)
			printf(">>fiber-%d, send: %lu %s\r\n",
				fiber_id(fiber), i, ret > 0 ? "ok" : "error");
	}
}

static void fiber_consumer(FIBER *fiber, void *ctx)
{
	CHANNEL *chan = (CHANNEL *) ctx;
	unsigned long i;

	for (i = 0; i < __max; i++) {
		unsigned long n = channel_recvul(chan);
		if (i < 10)
			printf(">>fiber-%d, recv: %lu\r\n", fiber_id(fiber), n);
	}
}

static void usage(const char *procname)
{
	printf("usage: %s -h [help] -n count -s nsenders -r nreceivers\r\n", procname);
}

int main(int argc, char *argv[])
{
	int   ch, i, nsenders = 1, nreceivers = 1;
	CHANNEL *chan;

	while ((ch = getopt(argc, argv, "hn:s:r:")) > 0) {
		switch (ch) {
		case 'h':
			usage(argv[0]);
			return 0;
		case 'n':
			__max = (unsigned long) atol(optarg);
			break;
		case 's':
			nsenders = atoi(optarg);
			break;
		case 'r':
			nreceivers = atoi(optarg);
			break;
		default:
			break;
		}
	}

	printf("max_count: %lu\r\n", __max);

	chan = channel_create(sizeof(unsigned long), 100);

	for (i = 0; i < nsenders; i++)
		fiber_create(fiber_producer, chan, 32000);

	for (i = 0; i < nreceivers; i++)
		fiber_create(fiber_consumer, chan, 32000);

	fiber_schedule();

	channel_free(chan);

	return 0;
}
