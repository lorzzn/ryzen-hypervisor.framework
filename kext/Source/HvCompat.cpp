#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include "HvCompat.hpp"
#include "Log.hpp"
#include "../../include/rhv_client_install.hpp"
#include "../../build/HyperKitPayload.h"

namespace rhv { namespace hv { namespace patch {
namespace {

IOLock *gInstallLock = nullptr;
bool gDisabled = false;

struct TaskMemory {
    vm_map_t map;

    static bool read(void *, uint64_t address, void *out, size_t size)
    {
        return copyin((user_addr_t)address, out, (user_size_t)size) == 0;
    }

    static bool write(void *, uint64_t address, const void *bytes, size_t size)
    {
        return copyout(bytes, (user_addr_t)address, (user_size_t)size) == 0;
    }

    static bool allocate(void *context, uint64_t size, uint64_t *out)
    {
        mach_vm_address_t address = 0;
        kern_return_t result = mach_vm_allocate(static_cast<TaskMemory *>(context)->map,
                                                &address, size, VM_FLAGS_ANYWHERE);
        *out = address;
        return result == KERN_SUCCESS;
    }

    static bool release(void *context, uint64_t address, uint64_t size)
    {
        return mach_vm_deallocate(static_cast<TaskMemory *>(context)->map,
                                  address, size) == KERN_SUCCESS;
    }

    static bool protect(void *context, uint64_t address, uint64_t size, uint32_t protection)
    {
        return vm_protect(static_cast<TaskMemory *>(context)->map, (vm_offset_t)address,
                          (vm_size_t)size, FALSE, (vm_prot_t)protection) == KERN_SUCCESS;
    }

    static bool writable(void *context, uint64_t page, uint32_t *old)
    {
        auto *self = static_cast<TaskMemory *>(context);
        mach_vm_address_t address = page;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = 0;
        if (mach_vm_region(self->map, &address, &size, VM_REGION_BASIC_INFO_64,
                          (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS ||
            !rhv::compat::contains(address, size, page, 4096)) return false;
        *old = (uint32_t)info.protection;
        return protect(context, page, 4096, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    }

    rhv::compat::Memory operations()
    {
        return {{this, read}, write, allocate, release, writable, protect};
    }
};

} // namespace

void startCompat()
{
    uint32_t enabled = 1;
    gDisabled = PE_parse_boot_argn("rhv_docker_compat", &enabled, sizeof(enabled)) && !enabled;
    if (!gDisabled && !gInstallLock)
        gInstallLock = IOLockAlloc();
}

void stopCompat()
{
    if (gInstallLock) {
        IOLockFree(gInstallLock);
        gInstallLock = nullptr;
    }
}

void installHyperKitCompat(vm_map_t map, mach_vm_address_t image)
{
    if (gDisabled || !gInstallLock || !image) return;
    TaskMemory memory{map};
    rhv::compat::Payload payload{rhv_payload_bytes, rhv_payload_size, rhv_payload_text_size,
        rhv_payload_config_offset, rhv_payload_open_offset, rhv_payload_connect_offset};
    uint64_t address = 0;
    IOLockLock(gInstallLock);
    auto result = rhv::compat::install(memory.operations(), image, payload, &address);
    IOLockUnlock(gInstallLock);
    using rhv::compat::InstallResult;
    switch (result) {
    case InstallResult::Installed:
        RHV_INFO("HyperKit compatibility: PID and fileserver hooks installed at %#llx (pid %d)",
                 (unsigned long long)address, proc_selfpid());
        break;
    case InstallResult::AlreadyInstalled:
        RHV_INFO("HyperKit compatibility: hooks already installed (pid %d)", proc_selfpid());
        break;
    case InstallResult::Unsupported:
        RHV_WARN("HyperKit compatibility: imports or payload unsupported (pid %d)", proc_selfpid());
        break;
    case InstallResult::Failed:
        RHV_WARN("HyperKit compatibility: installation failed and hooks rolled back (pid %d)",
                 proc_selfpid());
        break;
    case InstallResult::Partial:
        RHV_WARN("HyperKit compatibility: incomplete installation or protection restore at %#llx (pid %d)",
                 (unsigned long long)address, proc_selfpid());
        break;
    }
}

} } }
