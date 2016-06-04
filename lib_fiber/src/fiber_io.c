#include "stdafx.h"
#include <fcntl.h>
#define __USE_GNU
#include <dlfcn.h>
#include <sys/stat.h>
#include "event.h"
#include "fiber_schedule.h"
#include "fiber.h"
#include "fiber_io.h"

typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*readv_fn)(int, const struct iovec *, int);
typedef ssize_t (*recv_fn)(int, void *, size_t, int);
typedef ssize_t (*recvfrom_fn)(int, void *, size_t, int,
	struct sockaddr *, socklen_t *);
typedef ssize_t (*recvmsg_fn)(int, struct msghdr *, int);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*writev_fn)(int, const struct iovec *, int);
typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef ssize_t (*sendto_fn)(int, const void *, size_t, int,
	const struct sockaddr *, socklen_t);
typedef ssize_t (*sendmsg_fn)(int, const struct msghdr *, int);

static read_fn     __sys_read     = NULL;
static readv_fn    __sys_readv    = NULL;
static recv_fn     __sys_recv     = NULL;
static recvfrom_fn __sys_recvfrom = NULL;
static recvmsg_fn  __sys_recvmsg  = NULL;

static write_fn    __sys_write    = NULL;
static writev_fn   __sys_writev   = NULL;
static send_fn     __sys_send     = NULL;
static sendto_fn   __sys_sendto   = NULL;
static sendmsg_fn  __sys_sendmsg  = NULL;

static EVENT      *__event        = NULL;
static FIBER     **__io_fibers    = NULL;
static size_t      __io_count     = 0;
static FIBER      *__ev_fiber     = NULL;
static ACL_RING    __ev_timer;
static int         __sleeping_count;
static int         __io_stop      = 0;

static void fiber_io_loop(FIBER *fiber, void *ctx);

#define MAXFD		1024
#define STACK_SIZE	819200

void fiber_io_hook(void)
{
	static int __called = 0;

	if (__called)
		return;

	__called++;

	__sys_read     = (read_fn) dlsym(RTLD_NEXT, "read");
	__sys_readv    = (readv_fn) dlsym(RTLD_NEXT, "readv");
	__sys_recv     = (recv_fn) dlsym(RTLD_NEXT, "recv");
	__sys_recvfrom = (recvfrom_fn) dlsym(RTLD_NEXT, "recvfrom");
	__sys_recvmsg  = (recvmsg_fn) dlsym(RTLD_NEXT, "recvmsg");

	__sys_write    = (write_fn) dlsym(RTLD_NEXT, "write");
	__sys_writev   = (writev_fn) dlsym(RTLD_NEXT, "writev");
	__sys_send     = (send_fn) dlsym(RTLD_NEXT, "send");
	__sys_sendto   = (sendto_fn) dlsym(RTLD_NEXT, "sendto");
	__sys_sendmsg  = (sendmsg_fn) dlsym(RTLD_NEXT, "sendmsg");

	__event        = event_create(MAXFD);
	__io_fibers    = (FIBER **) acl_mycalloc(MAXFD, sizeof(FIBER *));

	acl_ring_init(&__ev_timer);

	fiber_net_hook();
}

void fiber_io_stop(void)
{
	__io_stop = 1;
}

#define RING_TO_FIBER(r) \
	((FIBER *) ((char *) (r) - offsetof(FIBER, me)))

#define FIRST_FIBER(head) \
	(acl_ring_succ(head) != (head) ? RING_TO_FIBER(acl_ring_succ(head)) : 0)

#define SET_TIME(x) {  \
	gettimeofday(&tv, NULL);  \
	(x) = tv.tv_sec * 1000 + tv.tv_usec / 1000; \
}

void fiber_io_check(void)
{
	if (__ev_fiber == NULL)
		__ev_fiber = fiber_create(fiber_io_loop, __event, STACK_SIZE);
}

void fiber_io_dec(void)
{
	__io_count--;
}

void fiber_io_inc(void)
{
	__io_count++;
}

EVENT *fiber_io_event(void)
{
	return __event;
}

static void fiber_io_loop(FIBER *self acl_unused, void *ctx)
{
	EVENT *ev = (EVENT *) ctx;
	int timer_left;
	FIBER *fiber;
	int now, last = 0;
	struct timeval tv;

	fiber_system();

	for (;;) {
		while (fiber_yield() > 0) {}

		fiber = FIRST_FIBER(&__ev_timer);

		if (fiber == NULL)
			timer_left = -1;
		else {
			SET_TIME(now);
			last = now;
			if (now >= fiber->when)
				timer_left = 0;
			else
				timer_left = fiber->when - now;
		}

		/* add 1 just for the deviation of epoll_wait */
		event_process(ev, timer_left > 0 ?
			timer_left + 1 : timer_left);

		if (__io_count == 0 && __io_stop)
			break;

		if (fiber == NULL)
			continue;

		SET_TIME(now);

		if (now - last < timer_left)
			continue;

		do {
			acl_ring_detach(&fiber->me);

			if (!fiber->sys && --__sleeping_count == 0)
				fiber_count_dec();

			fiber_ready(fiber);
			fiber = FIRST_FIBER(&__ev_timer);
		} while (fiber != NULL && now >= fiber->when);
	}
}

int fiber_delay(int n)
{
	int when, now;
	struct timeval tv;
	FIBER *fiber, *next = NULL;
	ACL_RING_ITER iter;

	if (__ev_fiber == NULL)
		__ev_fiber = fiber_create(fiber_io_loop, __event, STACK_SIZE);

	SET_TIME(when);
	when += n;

	acl_ring_foreach(iter, &__ev_timer) {
		fiber = acl_ring_to_appl(iter.ptr, FIBER, me);
		if (fiber->when >= when) {
			next = fiber;
			break;
		}
	}

	fiber = fiber_running();
	fiber->when = when;
	acl_ring_detach(&fiber->me);

	if (next)
		acl_ring_prepend(&next->me, &fiber->me);
	else
		acl_ring_prepend(&__ev_timer, &fiber->me);

	if (!fiber->sys && __sleeping_count++ == 0)
		fiber_count_inc();

	fiber_switch();

	SET_TIME(now);

	now -= when;
	return now < 0 ? 0 : now;
}

unsigned int sleep(unsigned int seconds)
{
	return fiber_delay(seconds * 1000) / 1000;
}

static void read_callback(EVENT *ev, int fd, void *ctx acl_unused, int mask)
{
	event_del(ev, fd, mask);
	fiber_ready(__io_fibers[fd]);

	__io_count--;
	__io_fibers[fd] = __io_fibers[__io_count];
}

void fiber_wait_read(int fd)
{
	if (__ev_fiber == NULL)
		__ev_fiber = fiber_create(fiber_io_loop, __event, STACK_SIZE);

	event_add(__event, fd, EVENT_READABLE, read_callback, NULL);

	__io_fibers[fd] = fiber_running();
	__io_count++;

	fiber_switch();
}

static void write_callback(EVENT *ev, int fd, void *ctx acl_unused, int mask)
{
	event_del(ev, fd, mask);
	fiber_ready(__io_fibers[fd]);

	__io_count--;
	__io_fibers[fd] = __io_fibers[__io_count];
}

void fiber_wait_write(int fd)
{
	if (__ev_fiber == NULL)
		__ev_fiber = fiber_create(fiber_io_loop, __event, STACK_SIZE);

	event_add(__event, fd, EVENT_WRITABLE, write_callback, NULL);

	__io_fibers[fd] = fiber_running();
	__io_count++;

	fiber_switch();
}

ssize_t read(int fd, void *buf, size_t count)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(fd, ACL_NON_BLOCKING);
#endif

	fiber_wait_read(fd);
	return __sys_read(fd, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(fd, ACL_NON_BLOCKING);
#endif

	fiber_wait_read(fd);
	return __sys_readv(fd, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(sockfd, ACL_NON_BLOCKING);
#endif

	fiber_wait_read(sockfd);
	return __sys_recv(sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
	struct sockaddr *src_addr, socklen_t *addrlen)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(sockfd, ACL_NON_BLOCKING);
#endif

	fiber_wait_read(sockfd);
	return __sys_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(sockfd, ACL_NON_BLOCKING);
#endif

	fiber_wait_read(sockfd);
	return __sys_recvmsg(sockfd, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(fd, ACL_NON_BLOCKING);
#endif

	while (1) {
		ssize_t n = __sys_write(fd, buf, count);

		if (n >= 0)
			return n;

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN)
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
			return -1;

		fiber_wait_write(fd);
	}
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(fd, ACL_NON_BLOCKING);
#endif

	while (1) {
		ssize_t n = __sys_writev(fd, iov, iovcnt);

		if (n >= 0)
			return n;

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN)
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
			return -1;

		fiber_wait_write(fd);
	}
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(sockfd, ACL_NON_BLOCKING);
#endif

	while (1) {
		ssize_t n = __sys_send(sockfd, buf, len, flags);

		if (n >= 0)
			return n;

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN)
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
			return -1;

		fiber_wait_write(sockfd);
	}
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
	const struct sockaddr *dest_addr, socklen_t addrlen)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(sockfd, ACL_NON_BLOCKING);
#endif

	while (1) {
		ssize_t n = __sys_sendto(sockfd, buf, len, flags,
				dest_addr, addrlen);

		if (n >= 0)
			return n;

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN)
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
			return -1;

		fiber_wait_write(sockfd);
	}
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
#ifdef SET_NONBLOCK
	acl_non_blocking(sockfd, ACL_NON_BLOCKING);
#endif

	while (1) {
		ssize_t n = __sys_sendmsg(sockfd, msg, flags);

		if (n >= 0)
			return n;

#if EAGAIN == EWOULDBLOCK
		if (errno != EAGAIN)
#else
		if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
			return -1;

		fiber_wait_write(sockfd);
	}
}