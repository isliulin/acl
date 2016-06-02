#ifndef FIBER_IO_INCLUDE_H
#define FIBER_IO_INCLUDE_H

#ifdef __cplusplus
extern "C" {
#endif

void fiber_io_hook(void);
void fiber_io_stop(void);
acl_int64 fiber_delay(acl_int64 n);

void fiber_set_dns(const char* ip, int port);

#ifdef __cplusplus
}
#endif

#endif
