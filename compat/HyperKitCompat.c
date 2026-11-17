/*
 * Relocation-free code embedded in RyzenHV, executed only inside HyperKit.
 * Calls go through dlsym so replacing the executable's import pointers cannot
 * recurse through those same pointers. No background process is required.
 */
#include "HyperKitCompat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct RhvCompatConfig rhv_compat_config = {
    RHV_COMPAT_MAGIC, RHV_COMPAT_ABI, 0, -1, 0, 0
};

typedef int (*OpenFunction)(const char *, int, ...);
typedef int (*ConnectFunction)(int, const struct sockaddr *, socklen_t);

struct Api {
    OpenFunction open;
    ConnectFunction connect;
    int (*close)(int);
    int (*flock)(int, int);
    int (*fstat)(int, struct stat *);
    ssize_t (*read)(int, void *, size_t);
    int (*truncate)(int, off_t);
    off_t (*seek)(int, off_t, int);
    int (*sysctl)(int *, unsigned, void *, size_t *, void *, size_t);
    pid_t (*getpid)(void);
    uid_t (*geteuid)(void);
    int (*socket)(int, int, int);
    int (*socketpair)(int, int, int, int *);
    int (*fcntl)(int, int, ...);
    int (*poll)(struct pollfd *, nfds_t, int);
    int (*getsockopt)(int, int, int, void *, socklen_t *);
    ssize_t (*sendmsg)(int, const struct msghdr *, int);
    ssize_t (*recv)(int, void *, size_t, int);
    int (*dup2)(int, int);
    int (*clock_gettime)(clockid_t, struct timespec *);
    int *(*error)(void);
};

static void *symbol(const char *name)
{
    typedef void *(*Lookup)(void *, const char *);
    return ((Lookup)rhv_compat_config.lookup)((void *)-2, name); /* RTLD_DEFAULT */
}

static int fail(int error)
{
    int *(*get_error)(void) = symbol("__error");
    *get_error() = error;
    return -1;
}

static struct Api load_api(void)
{
    struct Api api;
    api.open = symbol("open");
    api.connect = symbol("connect");
    api.close = symbol("close");
    api.flock = symbol("flock");
    api.fstat = symbol("fstat$INODE64");
    api.read = symbol("read");
    api.truncate = symbol("ftruncate");
    api.seek = symbol("lseek");
    api.sysctl = symbol("sysctl");
    api.getpid = symbol("getpid");
    api.geteuid = symbol("geteuid");
    api.socket = symbol("socket");
    api.socketpair = symbol("socketpair");
    api.fcntl = symbol("fcntl");
    api.poll = symbol("poll");
    api.getsockopt = symbol("getsockopt");
    api.sendmsg = symbol("sendmsg");
    api.recv = symbol("recv");
    api.dup2 = symbol("dup2");
    api.clock_gettime = symbol("clock_gettime");
    api.error = symbol("__error");
    return api;
}

static size_t text_length(const char *text, size_t limit)
{
    size_t n = 0;
    while (n < limit && text[n]) n++;
    return n;
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int ends_with(const char *text, const char *suffix)
{
    size_t n = text_length(text, PATH_MAX);
    size_t s = text_length(suffix, PATH_MAX);
    return n < PATH_MAX && n >= s && equal(text + n - s, suffix);
}

static void copy_bytes(void *destination, const void *source, size_t size)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < size; i++) out[i] = in[i];
}

/* -1 means the owner cannot be determined, so the file must be preserved. */
static int owns_vm(const struct Api *api, pid_t pid, const char *pidfile)
{
    if (pid == api->getpid()) return 0;
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc info;
    size_t size = sizeof(info);
    if (api->sysctl(mib, 4, &info, &size, NULL, 0)) return -1;
    if (!size) return 0;
    if (size != sizeof(info)) return -1;
    const char name[] = "com.docker.hyper";
    for (size_t i = 0; i < sizeof(name) - 1; i++)
        if (info.kp_proc.p_comm[i] != name[i]) return 0;

    char arguments[4096];
    int args_mib[] = {CTL_KERN, KERN_PROCARGS2, pid};
    size = sizeof(arguments);
    if (api->sysctl(args_mib, 3, arguments, &size, NULL, 0) ||
        size <= sizeof(int) || size > sizeof(arguments)) return -1;
    int argc;
    copy_bytes(&argc, arguments, sizeof(argc));
    if (argc < 1 || argc > 1024) return -1;
    size_t at = sizeof(argc);
    while (at < size && arguments[at]) at++;
    while (at < size && !arguments[at]) at++;
    int next_is_path = 0;
    for (int i = 0; i < argc && at < size; i++) {
        const char *arg = arguments + at;
        size_t length = text_length(arg, size - at);
        if (length == size - at) return -1;
        if (next_is_path && equal(arg, pidfile)) return 1;
        next_is_path = equal(arg, "-F");
        at += length + 1;
    }
    return 0;
}

static pid_t read_pid(const char *bytes, size_t size)
{
    uint64_t value = 0;
    size_t i = 0;
    while (i < size && bytes[i] >= '0' && bytes[i] <= '9') {
        value = value * 10 + (unsigned)(bytes[i++] - '0');
        if (value > INT_MAX) return 0;
    }
    if (!i || !value) return 0;
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\n' || bytes[i] == '\r')) i++;
    return i == size ? (pid_t)value : 0;
}

static int safe_file(const struct Api *api, int fd)
{
    struct stat status;
    return api->fstat && !api->fstat(fd, &status) &&
           S_ISREG(status.st_mode) && status.st_uid == api->geteuid() &&
           status.st_nlink == 1;
}

int rhv_compat_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    OpenFunction original = symbol("open");
    const int pid_flags = O_CREAT | O_EXCL | O_WRONLY;
    if (!path || (flags & (O_CREAT | O_EXCL | O_ACCMODE)) != pid_flags ||
        !ends_with(path, "/hyperkit.pid")) return original(path, flags, mode);

    if (!__sync_bool_compare_and_swap(&rhv_compat_config.pid_lock_fd, -1, -2))
        return fail(EBUSY);
    struct Api api = load_api();
    int lock = -1, file = -1, error = EINVAL;
    char lock_path[PATH_MAX];
    size_t length = text_length(path, sizeof(lock_path));
    const char suffix[] = ".rhv-lock";
    if (length + sizeof(suffix) > sizeof(lock_path)) {
        error = ENAMETOOLONG;
        goto failed;
    }
    copy_bytes(lock_path, path, length);
    copy_bytes(lock_path + length, suffix, sizeof(suffix));
    lock = api.open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) { error = *api.error(); goto failed; }
    if (!safe_file(&api, lock)) { error = EPERM; goto failed; }
    if (api.flock(lock, LOCK_EX | LOCK_NB)) { error = EBUSY; goto failed; }

    file = api.open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, mode);
    if (file < 0) { error = *api.error(); goto failed; }
    if (!safe_file(&api, file)) { error = EPERM; goto failed; }
    char previous[64];
    ssize_t count = api.read(file, previous, sizeof(previous));
    if (count < 0) { error = *api.error(); goto failed; }
    pid_t pid = read_pid(previous, (size_t)count);
    if (pid && owns_vm(&api, pid, path) != 0) { error = EBUSY; goto failed; }
    if (api.truncate(file, 0) || api.seek(file, 0, SEEK_SET) < 0) {
        error = *api.error();
        goto failed;
    }
    rhv_compat_config.pid_lock_fd = lock;
    if (count) rhv_compat_config.pid_recoveries++;
    return file;

failed:
    if (file >= 0) api.close(file);
    if (lock >= 0) api.close(lock);
    rhv_compat_config.pid_lock_fd = -1;
    return fail(error);
}

static int64_t milliseconds(const struct Api *api)
{
    struct timespec now;
    if (api->clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_socket(const struct Api *api, int fd, short events, int64_t deadline)
{
    for (;;) {
        int64_t now = milliseconds(api);
        if (now < 0 || now >= deadline) return fail(ETIMEDOUT);
        struct pollfd request = {.fd = fd, .events = events};
        int result = api->poll(&request, 1, (int)(deadline - now));
        if (result > 0 && (request.revents & events)) return 0;
        if (result < 0 && *api->error() == EINTR) continue;
        return fail(result == 0 ? ETIMEDOUT : ECONNRESET);
    }
}

static int fileserver_path(const struct sockaddr *address, socklen_t length,
                           struct sockaddr_un *service)
{
    if (!address || address->sa_family != AF_UNIX ||
        length < offsetof(struct sockaddr_un, sun_path) + 1) return 0;
    const struct sockaddr_un *local = (const struct sockaddr_un *)address;
    size_t capacity = length - offsetof(struct sockaddr_un, sun_path);
    if (capacity > sizeof(local->sun_path)) capacity = sizeof(local->sun_path);
    size_t size = text_length(local->sun_path, capacity);
    if (size == capacity || !ends_with(local->sun_path, "/00000002.00001003")) return 0;

    size_t end = size - sizeof("/00000002.00001003") + 1;
    size_t vm = end;
    while (vm && local->sun_path[vm - 1] >= '0' && local->sun_path[vm - 1] <= '9') vm--;
    if (vm == end || vm < 5) return 0;
    const char prefix[] = "/vms/";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++)
        if (local->sun_path[vm - 5 + i] != prefix[i]) return 0;
    size_t base = vm - 5;
    const char name[] = "/filesystem-fd.sock";
    if (base + sizeof(name) > sizeof(service->sun_path)) return 0;
    service->sun_len = sizeof(*service);
    service->sun_family = AF_UNIX;
    copy_bytes(service->sun_path, local->sun_path, base);
    copy_bytes(service->sun_path + base, name, sizeof(name));
    return 1;
}

int rhv_compat_connect(int fd, const struct sockaddr *address, socklen_t length)
{
    ConnectFunction original = symbol("connect");
    int result = original(fd, address, length);
    if (!result) return 0;
    int *(*get_error)(void) = symbol("__error");
    int original_error = *get_error();
    struct sockaddr_un service = {0};
    if ((original_error != ENOENT && original_error != ECONNREFUSED) ||
        !fileserver_path(address, length, &service)) return fail(original_error);

    struct Api api = load_api();
    int pair[2] = {-1, -1}, control = -1, error = EIO;
    int descriptor_flags = api.fcntl(fd, F_GETFD);
    if (descriptor_flags < 0) return -1;
    int64_t now = milliseconds(&api);
    if (now < 0) return fail(EIO);
    int64_t deadline = now + 3000;
    if (api.socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
    control = api.socket(AF_UNIX, SOCK_STREAM, 0);
    if (control < 0) { error = *api.error(); goto failed; }
    if (api.fcntl(control, F_SETFL, O_NONBLOCK)) { error = *api.error(); goto failed; }
    if (api.connect(control, (const struct sockaddr *)&service, sizeof(service))) {
        if (*api.error() != EINPROGRESS ||
            wait_socket(&api, control, POLLOUT, deadline)) {
            error = *api.error();
            goto failed;
        }
        int pending = 0;
        socklen_t pending_length = sizeof(pending);
        if (api.getsockopt(control, SOL_SOCKET, SO_ERROR, &pending, &pending_length) || pending) {
            error = pending ? pending : *api.error();
            goto failed;
        }
    }
    unsigned char byte = 0;
    struct iovec vector = {.iov_base = &byte, .iov_len = 1};
    union {
        struct cmsghdr aligned;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } ancillary = {0};
    struct msghdr message = {0};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.bytes;
    message.msg_controllen = sizeof(ancillary.bytes);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    copy_bytes(CMSG_DATA(header), &pair[1], sizeof(pair[1]));
    if (api.sendmsg(control, &message, MSG_DONTWAIT) != 1) {
        error = *api.error();
        goto failed;
    }
    if (wait_socket(&api, control, POLLIN, deadline)) { error = *api.error(); goto failed; }
    if (api.recv(control, &byte, 1, MSG_DONTWAIT) != 1 || byte != 0xfd) {
        error = EPROTO;
        goto failed;
    }
    if (api.dup2(pair[0], fd) < 0 || api.fcntl(fd, F_SETFD, descriptor_flags)) {
        error = *api.error();
        goto failed;
    }
    api.close(pair[0]);
    api.close(pair[1]);
    api.close(control);
    rhv_compat_config.fileshare_handoffs++;
    return 0;

failed:
    if (pair[0] >= 0) api.close(pair[0]);
    if (pair[1] >= 0) api.close(pair[1]);
    if (control >= 0) api.close(control);
    return fail(error);
}
