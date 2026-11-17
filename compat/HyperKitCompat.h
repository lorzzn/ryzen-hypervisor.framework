#ifndef RHV_HYPERKIT_COMPAT_H
#define RHV_HYPERKIT_COMPAT_H

#include <stdint.h>

#define RHV_COMPAT_MAGIC UINT64_C(0x524856434f4d5031)
#define RHV_COMPAT_ABI 1

struct RhvCompatConfig {
    uint64_t magic;
    uint64_t abi;
    uintptr_t lookup;
    int64_t pid_lock_fd;
    uint64_t pid_recoveries;
    uint64_t fileshare_handoffs;
};

#ifndef KERNEL
#include <sys/socket.h>

extern struct RhvCompatConfig rhv_compat_config;
int rhv_compat_open(const char *path, int flags, ...);
int rhv_compat_connect(int fd, const struct sockaddr *address, socklen_t length);
#endif

#endif
