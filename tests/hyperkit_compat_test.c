#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef RHV_BASELINE
#define rhv_compat_open open
#define rhv_compat_connect connect
#else
#include "../compat/HyperKitCompat.h"
#ifdef RHV_MAPPED_PAYLOAD
#include "../build/HyperKitPayload.h"
#include <sys/mman.h>
static int (*mapped_open)(const char *, int, ...);
static int (*mapped_connect)(int, const struct sockaddr *, socklen_t);
#define rhv_compat_open mapped_open
#define rhv_compat_connect mapped_connect
#endif

/* A live HyperKit with a selected VM, without starting another real VM. */
static int test_sysctl(int *mib, unsigned count, void *out, size_t *size,
                       void *value, size_t value_size)
{
    const char *selected = getenv("RHV_TEST_ACTIVE_PID");
    if (selected && count >= 3 && mib[count - 1] == atoi(selected)) {
        if (count == 4 && mib[1] == KERN_PROC && mib[2] == KERN_PROC_PID) {
            struct kinfo_proc info = {0};
            memcpy(info.kp_proc.p_comm, "com.docker.hyper", 16);
            if (*size < sizeof(info)) return -1;
            memcpy(out, &info, sizeof(info));
            *size = sizeof(info);
            return 0;
        }
        if (count == 3 && mib[1] == KERN_PROCARGS2) {
            const char *path = getenv("RHV_TEST_ACTIVE_PATH");
            const char executable[] = "/Applications/Docker.app/Contents/Resources/bin/com.docker.hyperkit";
            unsigned char bytes[4096] = {0};
            int attached = getenv("RHV_TEST_ATTACHED_F") != NULL;
            int argc = attached ? 2 : 3;
            size_t at = sizeof(argc);
            memcpy(bytes, &argc, sizeof(argc));
            memcpy(bytes + at, executable, sizeof(executable));
            at += sizeof(executable) + 1;
            memcpy(bytes + at, executable, sizeof(executable));
            at += sizeof(executable);
            memcpy(bytes + at, "-F", attached ? 2 : 3);
            at += attached ? 2 : 3;
            memcpy(bytes + at, path, strlen(path) + 1);
            at += strlen(path) + 1;
            if (*size < at) return -1;
            memcpy(out, bytes, at);
            *size = at;
            return 0;
        }
    }
    return sysctl(mib, count, out, size, value, value_size);
}

static void *lookup(void *handle, const char *name)
{
    const char *missing = getenv("RHV_TEST_MISSING_SYMBOL");
    if (missing && !strcmp(missing, name)) return NULL;
    if (!strcmp(name, "sysctl")) return (void *)test_sysctl;
    return dlsym(handle, name);
}
#endif

static int connect_file(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int nonblocking = getenv("RHV_TEST_NONBLOCK") != NULL;
    if (nonblocking && (fcntl(fd, F_SETFL, O_NONBLOCK) ||
                        fcntl(fd, F_SETFD, FD_CLOEXEC))) return 14;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) return 10;
    strcpy(address.sun_path, path);
    if (rhv_compat_connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        printf("ERR %d\n", errno);
        close(fd);
        return 2;
    }
    if (nonblocking) {
        if (!(fcntl(fd, F_GETFL) & O_NONBLOCK) || !(fcntl(fd, F_GETFD) & FD_CLOEXEC)) {
            close(fd);
            return 15;
        }
        if (fcntl(fd, F_SETFL, 0)) return 16;
    }
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char reply[4];
    if (write(fd, "ping", 4) != 4 || read(fd, reply, 4) != 4 ||
        memcmp(reply, "pong", 4)) {
        close(fd);
        return 3;
    }
    close(fd);
    puts("OK");
    return 0;
}

int main(int argc, char **argv)
{
#ifndef RHV_BASELINE
    struct RhvCompatConfig *configuration;
#ifdef RHV_MAPPED_PAYLOAD
    unsigned char *mapped = mmap(NULL, rhv_payload_size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) return 11;
    memcpy(mapped, rhv_payload_bytes, rhv_payload_size);
    configuration = (void *)(mapped + rhv_payload_config_offset);
    mapped_open = (void *)(mapped + rhv_payload_open_offset);
    mapped_connect = (void *)(mapped + rhv_payload_connect_offset);
    if (mprotect(mapped, rhv_payload_text_size, PROT_READ | PROT_EXEC)) return 12;
#else
    configuration = &rhv_compat_config;
#endif
    if (configuration->magic != RHV_COMPAT_MAGIC || configuration->abi != RHV_COMPAT_ABI)
        return 13;
    configuration->lookup = (uintptr_t)lookup;
    configuration->pid_lock_fd = -1;
#endif
    if (argc < 3) return 10;
    if (!strcmp(argv[1], "connect")) return connect_file(argv[2]);
    if (!strcmp(argv[1], "pid") || !strcmp(argv[1], "hold")) {
        int fd = rhv_compat_open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) {
            printf("ERR %d\n", errno);
            return 2;
        }
        char pid[32];
        int length = snprintf(pid, sizeof(pid), "%d", getpid());
        if (write(fd, pid, (size_t)length) != length) return 3;
        close(fd);
        printf("OK %d\n", getpid());
        fflush(stdout);
        if (!strcmp(argv[1], "hold")) (void)getchar();
        return 0;
    }
    return 10;
}
