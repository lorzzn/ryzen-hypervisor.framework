#define main image_parser_tests
#include "client_image_test.cpp"
#undef main
#include "../include/rhv_client_install.hpp"

struct InstallImage : Image {
    std::vector<unsigned char> payload = std::vector<unsigned char>(0x3000);
    bool allocated = false;
    unsigned writes = 0, failWrite = 0, failSecondWrite = 0;
    bool partialWrite = false;
    bool failRestore = false;
    uint32_t protections[3] = {1, 1, 1};
    static constexpr uint64_t allocatedBase = base + 0x10000;

    InstallImage()
    {
        put<uint64_t>(0x1000, 0x710000001);
        put<uint64_t>(0x1008, 0x710000002);
    }

    static bool read(void *context, uint64_t address, void *out, size_t size)
    {
        auto *self = static_cast<InstallImage *>(context);
        if (address >= allocatedBase && address - allocatedBase <= self->payload.size() &&
            size <= self->payload.size() - (address - allocatedBase)) {
            std::memcpy(out, self->payload.data() + address - allocatedBase, size);
            return self->allocated;
        }
        return Image::read(context, address, out, size);
    }

    static bool write(void *context, uint64_t address, const void *data, size_t size)
    {
        auto *self = static_cast<InstallImage *>(context);
        self->writes++;
        if (self->writes == self->failWrite || self->writes == self->failSecondWrite) {
            if (self->partialWrite && rhv::compat::contains(base, self->bytes.size(), address, size))
                std::memcpy(self->bytes.data() + address - base, data, size / 2);
            return false;
        }
        if (address >= allocatedBase && address - allocatedBase <= self->payload.size() &&
            size <= self->payload.size() - (address - allocatedBase)) {
            std::memcpy(self->payload.data() + address - allocatedBase, data, size);
            return self->allocated;
        }
        if (!rhv::compat::contains(base, self->bytes.size(), address, size)) return false;
        std::memcpy(self->bytes.data() + address - base, data, size);
        return true;
    }

    static bool allocate(void *context, uint64_t size, uint64_t *address)
    {
        auto *self = static_cast<InstallImage *>(context);
        if (self->allocated || size > self->payload.size()) return false;
        self->allocated = true;
        *address = allocatedBase;
        return true;
    }

    static bool release(void *context, uint64_t address, uint64_t)
    {
        auto *self = static_cast<InstallImage *>(context);
        assert(address == allocatedBase);
        self->allocated = false;
        return true;
    }

    static bool writable(void *context, uint64_t address, uint32_t *old)
    {
        auto *self = static_cast<InstallImage *>(context);
        if (!rhv::compat::contains(base, self->bytes.size(), address, 4096)) return false;
        unsigned page = (unsigned)((address - base) / 4096);
        *old = self->protections[page];
        self->protections[page] = 3;
        return true;
    }

    static bool protect(void *context, uint64_t address, uint64_t size, uint32_t protection)
    {
        auto *self = static_cast<InstallImage *>(context);
        if (address == allocatedBase) return size == 4096 && protection == 5;
        if (self->failRestore) return false;
        if (!rhv::compat::contains(base, self->bytes.size(), address, size)) return false;
        self->protections[(address - base) / 4096] = protection;
        return true;
    }
    rhv::compat::Memory memory()
    {
        return {{this, read}, write, allocate, release, writable, protect};
    }

    uint64_t pointer(size_t at)
    {
        uint64_t value;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    }
};

int main()
{
    unsigned char bytes[8192] = {0};
    RhvCompatConfig config = {RHV_COMPAT_MAGIC, RHV_COMPAT_ABI, 0, -1, 0, 0};
    std::memcpy(bytes + 4096, &config, sizeof(config));
    rhv::compat::Payload payload{bytes, sizeof(bytes), 4096, 4096, 256, 512};
    uint64_t address = 0;
    InstallImage image;
    assert(rhv::compat::install(image.memory(), base, payload, &address) ==
           rhv::compat::InstallResult::Installed);
    assert(address == InstallImage::allocatedBase);
    assert(image.pointer(0x1000) == address + 256);
    assert(image.pointer(0x1008) == address + 512);
    assert(image.protections[1] == 1);
    unsigned writes = image.writes;
    assert(rhv::compat::install(image.memory(), base, payload, &address) ==
           rhv::compat::InstallResult::AlreadyInstalled);
    assert(image.writes == writes);

    for (unsigned fail = 1; fail <= 4; fail++) {
        InstallImage failure;
        failure.failWrite = fail;
        assert(rhv::compat::install(failure.memory(), base, payload, &address) ==
               rhv::compat::InstallResult::Failed);
        assert(failure.pointer(0x1000) == 0x710000001);
        assert(failure.pointer(0x1008) == 0x710000002);
        assert(!failure.allocated);
        assert(failure.protections[1] == 1);
    }
    for (unsigned fail = 3; fail <= 4; fail++) {
        InstallImage partial;
        partial.failWrite = fail;
        partial.partialWrite = true;
        assert(rhv::compat::install(partial.memory(), base, payload, &address) ==
               rhv::compat::InstallResult::Failed);
        assert(partial.pointer(0x1000) == 0x710000001);
        assert(partial.pointer(0x1008) == 0x710000002);
        assert(!partial.allocated);
        assert(partial.protections[1] == 1);
    }
    InstallImage failed_rollback;
    failed_rollback.failWrite = 4;
    failed_rollback.failSecondWrite = 5;
    assert(rhv::compat::install(failed_rollback.memory(), base, payload, &address) ==
           rhv::compat::InstallResult::Partial);
    assert(failed_rollback.allocated && address == InstallImage::allocatedBase);
    assert(failed_rollback.pointer(0x1000) == address + 256);
    assert(failed_rollback.pointer(0x1008) == 0x710000002);

    InstallImage failed_restore;
    failed_restore.failRestore = true;
    assert(rhv::compat::install(failed_restore.memory(), base, payload, &address) ==
           rhv::compat::InstallResult::Partial);
    assert(failed_restore.allocated && address == InstallImage::allocatedBase);
    assert(failed_restore.pointer(0x1000) == address + 256);
    assert(failed_restore.pointer(0x1008) == address + 512);

    InstallImage malformed;
    malformed.put<uint32_t>(0, 0);
    assert(rhv::compat::install(malformed.memory(), base, payload, &address) ==
           rhv::compat::InstallResult::Unsupported);
    assert(!malformed.allocated && !malformed.writes);
    std::puts("client_install_test: install, idempotency and fault handling passed");
}
