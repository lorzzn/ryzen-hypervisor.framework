/* Test the exact installer against a real task. No files are patched on disk. */
#include "../include/rhv_client_install.hpp"
#include "../build/HyperKitPayload.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <unistd.h>

struct TaskMemory {
    mach_port_t task;

    static bool read(void *context, uint64_t address, void *out, size_t size)
    {
        auto *self = static_cast<TaskMemory *>(context);
        mach_vm_size_t copied = 0;
        return mach_vm_read_overwrite(self->task, address, size,
               (mach_vm_address_t)out, &copied) == KERN_SUCCESS && copied == size;
    }

    static bool write(void *context, uint64_t address, const void *bytes, size_t size)
    {
        return mach_vm_write(static_cast<TaskMemory *>(context)->task, address,
                             (vm_offset_t)bytes, (mach_msg_type_number_t)size) == KERN_SUCCESS;
    }

    static bool allocate(void *context, uint64_t size, uint64_t *out)
    {
        mach_vm_address_t address = 0;
        kern_return_t result = mach_vm_allocate(static_cast<TaskMemory *>(context)->task,
                                                &address, size, VM_FLAGS_ANYWHERE);
        *out = address;
        return result == KERN_SUCCESS;
    }

    static bool release(void *context, uint64_t address, uint64_t size)
    {
        return mach_vm_deallocate(static_cast<TaskMemory *>(context)->task,
                                  address, size) == KERN_SUCCESS;
    }

    static bool protect(void *context, uint64_t address, uint64_t size, uint32_t protection)
    {
        return mach_vm_protect(static_cast<TaskMemory *>(context)->task,
                              address, size, false, protection) == KERN_SUCCESS;
    }

    static bool writable(void *context, uint64_t page, uint32_t *old)
    {
        auto *self = static_cast<TaskMemory *>(context);
        mach_vm_address_t address = page;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = mach_vm_region(self->task, &address, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || address > page || size < page - address + 4096)
            return false;
        *old = info.protection;
        return protect(context, page, 4096, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    }

    rhv::compat::Memory operations()
    {
        return {{this, read}, write, allocate, release, writable, protect};
    }
};

static uint64_t mainImage(TaskMemory &memory)
{
    task_dyld_info_data_t info{};
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if (task_info(memory.task, TASK_DYLD_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    struct DyldPrefix { uint32_t version, count; uint64_t images; } prefix{};
    struct Image { uint64_t header, path, modified; } image{};
    if (!TaskMemory::read(&memory, info.all_image_info_addr, &prefix, sizeof(prefix)) ||
        !prefix.count || !prefix.images ||
        !TaskMemory::read(&memory, prefix.images, &image, sizeof(image))) return 0;
    return image.header;
}

int main(int argc, char **argv)
{
    bool selfTest = argc == 1;
    pid_t pid = selfTest ? getpid() : (pid_t)strtol(argv[1], nullptr, 10);
    mach_port_t task = mach_task_self();
    if (!selfTest) {
        kern_return_t status = task_for_pid(mach_task_self(), pid, &task);
        if (status != KERN_SUCCESS) {
            std::printf("task_for_pid failed: %d\n", status);
            return 2;
        }
    }
    (void)connect(-1, nullptr, 0);
    if (!dlsym(RTLD_DEFAULT, "open")) return 3;
    char directory[] = "/tmp/rhv-live-XXXXXX";
    char pidfile[256] = {0};
    if (selfTest) {
        if (!mkdtemp(directory)) return 4;
        std::snprintf(pidfile, sizeof(pidfile), "%s/hyperkit.pid", directory);
        int old = open(pidfile, O_CREAT | O_WRONLY, 0600);
        if (old < 0 || write(old, "999999999", 9) != 9) return 5;
        close(old);
    }
    TaskMemory memory{task};
    uint64_t image = mainImage(memory);
    rhv::compat::Imports imports{};
    if (!image || !rhv::compat::findImports(memory.operations().reader, image, &imports)) {
        std::puts("import discovery failed");
        return 6;
    }
    uint64_t slots[8], before[8];
    unsigned slotCount = 0;
    for (unsigned i = 0; i < imports.openCount; i++) slots[slotCount++] = imports.openSlots[i];
    for (unsigned i = 0; i < imports.connectCount; i++) slots[slotCount++] = imports.connectSlots[i];
    for (unsigned i = 0; i < slotCount; i++)
        if (!TaskMemory::read(&memory, slots[i], &before[i], sizeof(uint64_t))) return 7;
    rhv::compat::Payload payload{rhv_payload_bytes, rhv_payload_size, rhv_payload_text_size,
        rhv_payload_config_offset, rhv_payload_open_offset, rhv_payload_connect_offset};
    uint64_t address = 0;
    auto result = rhv::compat::install(memory.operations(), image, payload, &address);
    std::printf("installation=%d payload=%#llx image=%#llx\n",
                (int)result, (unsigned long long)address, (unsigned long long)image);
    if (result != rhv::compat::InstallResult::Installed) return 8;
    if (selfTest) {
        int fd = open(pidfile, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) { std::perror("patched open"); return 9; }
        close(fd);
        auto repeat = rhv::compat::install(memory.operations(), image, payload, nullptr);
        if (repeat != rhv::compat::InstallResult::AlreadyInstalled) return 10;
        unlink(pidfile);
        char lock[280];
        std::snprintf(lock, sizeof(lock), "%s.rhv-lock", pidfile);
        unlink(lock);
        rmdir(directory);
        std::puts("client_install_live_test: stale PID recovered through installed import hook");
        return 0;
    }
    std::puts("READY");
    std::fflush(stdout);
    char line[64];
    while (std::fgets(line, sizeof(line), stdin)) {
        if (!std::strncmp(line, "restore", 7)) break;
        RhvCompatConfig config{};
        if (TaskMemory::read(&memory, address + rhv_payload_config_offset, &config, sizeof(config)))
            std::printf("recoveries=%llu handoffs=%llu\n",
                (unsigned long long)config.pid_recoveries,
                (unsigned long long)config.fileshare_handoffs);
        std::fflush(stdout);
    }
    for (unsigned i = 0; i < slotCount; i++) {
        uint64_t page = slots[i] & ~UINT64_C(4095);
        uint32_t protection = 0;
        if (!TaskMemory::writable(&memory, page, &protection) ||
            !TaskMemory::write(&memory, slots[i], &before[i], sizeof(uint64_t)) ||
            !TaskMemory::protect(&memory, page, 4096, protection)) return 11;
    }
    /* Keep the mapping until process exit, because another thread may be in it. */
    std::puts("RESTORED");
    return 0;
}
