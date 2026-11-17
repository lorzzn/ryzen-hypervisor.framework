#ifndef RHV_CLIENT_INSTALL_HPP
#define RHV_CLIENT_INSTALL_HPP

#include "rhv_client_image.hpp"
#include "../compat/HyperKitCompat.h"

namespace rhv { namespace compat {

struct Memory {
    Reader reader;
    bool (*write)(void *, uint64_t, const void *, size_t);
    bool (*allocate)(void *, uint64_t, uint64_t *);
    bool (*release)(void *, uint64_t, uint64_t);
    bool (*writable)(void *, uint64_t, uint32_t *);
    bool (*protect)(void *, uint64_t, uint64_t, uint32_t);
};

struct Payload {
    const unsigned char *bytes;
    uint64_t size, textSize, configOffset, openOffset, connectOffset;
};

enum class InstallResult { Unsupported, Failed, Installed, AlreadyInstalled, Partial };

inline InstallResult install(const Memory &memory, uint64_t image,
                             const Payload &payload, uint64_t *installedAddress)
{
    if (installedAddress) *installedAddress = 0;
    if (!payload.bytes || !payload.size || payload.size > 65536 ||
        payload.size % 4096 || !payload.textSize || payload.textSize % 4096 ||
        payload.textSize >= payload.size || payload.configOffset < payload.textSize ||
        !contains(0, payload.size, payload.configOffset, sizeof(RhvCompatConfig)) ||
        payload.openOffset >= payload.textSize || payload.connectOffset >= payload.textSize)
        return InstallResult::Unsupported;
    Imports imports{};
    if (!findImports(memory.reader, image, &imports)) return InstallResult::Unsupported;
    struct Slot { uint64_t address, before, after; };
    Slot slots[8];
    unsigned count = 0;
    for (unsigned i = 0; i < imports.openCount; i++)
        slots[count++] = {imports.openSlots[i], 0, payload.openOffset};
    for (unsigned i = 0; i < imports.connectCount; i++)
        slots[count++] = {imports.connectSlots[i], 0, payload.connectOffset};
    for (unsigned i = 0; i < count; i++) {
        if (!readAt(memory.reader, slots[i].address, &slots[i].before, sizeof(uint64_t)) ||
            !slots[i].before) return InstallResult::Unsupported;
        for (unsigned j = 0; j < i; j++)
            if (slots[i].address == slots[j].address) return InstallResult::Unsupported;
    }

    /* Repeated VM creation in the same process must retain its original payload. */
    if (slots[0].before >= payload.openOffset) {
        uint64_t candidate = slots[0].before - payload.openOffset;
        RhvCompatConfig existing;
        if (candidate % 4096 == 0 && candidate <= UINT64_MAX - payload.size &&
            readAt(memory.reader, candidate + payload.configOffset, &existing, sizeof(existing)) &&
            existing.magic == RHV_COMPAT_MAGIC && existing.abi == RHV_COMPAT_ABI &&
            existing.lookup == imports.lookupStub) {
            if (installedAddress) *installedAddress = candidate;
            for (unsigned i = 0; i < count; i++)
                if (slots[i].before != candidate + slots[i].after) return InstallResult::Partial;
            return InstallResult::AlreadyInstalled;
        }
    }

    RhvCompatConfig config;
    for (size_t i = 0; i < sizeof(config); i++)
        reinterpret_cast<unsigned char *>(&config)[i] = payload.bytes[payload.configOffset + i];
    if (config.magic != RHV_COMPAT_MAGIC || config.abi != RHV_COMPAT_ABI ||
        config.pid_lock_fd != -1) return InstallResult::Unsupported;
    config.lookup = imports.lookupStub;
    uint64_t address = 0;
    void *context = memory.reader.context;
    if (!memory.allocate(context, payload.size, &address)) return InstallResult::Failed;
    if (address % 4096 || address > UINT64_MAX - payload.size ||
        !memory.write(context, address, payload.bytes, (size_t)payload.size) ||
        !memory.write(context, address + payload.configOffset, &config, sizeof(config)) ||
        !memory.protect(context, address, payload.textSize, 5)) {
        memory.release(context, address, payload.size);
        return InstallResult::Failed;
    }

    struct Page { uint64_t address; uint32_t protection; };
    Page pages[8];
    unsigned pageCount = 0, written = 0, attempted = 0;
    bool success = true;
    for (unsigned i = 0; i < count && success; i++) {
        uint64_t page = slots[i].address & ~UINT64_C(4095);
        bool found = false;
        for (unsigned j = 0; j < pageCount; j++) found |= pages[j].address == page;
        if (!found) {
            uint32_t protection = 0;
            success = memory.writable(context, page, &protection);
            if (success) pages[pageCount++] = {page, protection};
        }
    }
    for (; written < count && success; written++) {
        uint64_t replacement = address + slots[written].after;
        attempted = written + 1;
        success = memory.write(context, slots[written].address, &replacement, sizeof(replacement));
        if (!success) break;
    }
    bool rollback = true;
    if (!success) {
        /* A failed copy may have changed part of the attempted pointer too. */
        for (unsigned i = 0; i < attempted; i++)
            rollback &= memory.write(context, slots[i].address, &slots[i].before, sizeof(uint64_t));
    }
    bool restored = true;
    for (unsigned i = 0; i < pageCount; i++)
        restored &= memory.protect(context, pages[i].address, 4096, pages[i].protection);
    if (!success && rollback) {
        memory.release(context, address, payload.size);
        return restored ? InstallResult::Failed : InstallResult::Partial;
    }
    /* A failed rollback retains valid code for any hook that still points at it. */
    if (installedAddress) *installedAddress = address;
    return success && restored ? InstallResult::Installed : InstallResult::Partial;
}

} }
#endif
