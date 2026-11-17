#include "../include/rhv_client_image.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static bool readMemory(void *, uint64_t address, void *out, size_t size)
{
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
                                 (mach_vm_address_t)out, &copied) == KERN_SUCCESS &&
           copied == size;
}

struct FileImage {
    std::vector<char> bytes;
    std::vector<rhv::compat::Segment> segments;
    uint64_t slide;

    static bool read(void *context, uint64_t address, void *out, size_t size)
    {
        auto &image = *static_cast<FileImage *>(context);
        for (const auto &segment : image.segments) {
            if (segment.address > UINT64_MAX - image.slide) return false;
            uint64_t start = segment.address + image.slide;
            if (!rhv::compat::contains(start, segment.fileSize, address, size)) continue;
            uint64_t offset = segment.fileOffset + address - start;
            if (!rhv::compat::contains(0, image.bytes.size(), offset, size)) return false;
            std::memcpy(out, image.bytes.data() + offset, size);
            return true;
        }
        return false;
    }
};

static void checkFile(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    FileImage image{{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}, {}, 0};
    rhv::compat::MachHeader header{};
    assert(image.bytes.size() >= sizeof(header));
    std::memcpy(&header, image.bytes.data(), sizeof(header));
    assert(header.magic == 0xfeedfacf && header.commandBytes <= image.bytes.size() - sizeof(header));
    size_t at = sizeof(header);
    uint64_t base = 0x180000000;
    for (unsigned i = 0; i < header.commands; i++) {
        rhv::compat::Command command{};
        assert(at <= image.bytes.size() - sizeof(command));
        std::memcpy(&command, image.bytes.data() + at, sizeof(command));
        assert(command.bytes >= sizeof(command) && command.bytes <= image.bytes.size() - at);
        if (command.kind == 0x19) {
            rhv::compat::Segment segment{};
            assert(command.bytes >= sizeof(segment));
            std::memcpy(&segment, image.bytes.data() + at, sizeof(segment));
            image.segments.push_back(segment);
            if (rhv::compat::sameName(segment.name, sizeof(segment.name), "__TEXT")) {
                assert(base >= segment.address);
                image.slide = base - segment.address;
            }
        }
        at += command.bytes;
    }
    rhv::compat::Imports imports{};
    assert(rhv::compat::findImports({&image, FileImage::read}, base, &imports));
    std::printf("client_image_live_test: actual HyperKit open=%u connect=%u dlsym=%#llx\n",
                imports.openCount, imports.connectCount, (unsigned long long)imports.lookupStub);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        checkFile(argv[1]);
        return 0;
    }
    /* Retain real import slots in the executable. */
    int file = open("/dev/null", O_RDONLY);
    if (file >= 0) close(file);
    (void)connect(-1, nullptr, 0);
    assert(dlsym(RTLD_DEFAULT, "open"));
    rhv::compat::Imports imports{};
    assert(rhv::compat::findImports({nullptr, readMemory},
           (uint64_t)_dyld_get_image_header(0), &imports));
    assert(imports.openCount && imports.connectCount && imports.lookupStub);
    std::printf("client_image_live_test: open=%u connect=%u dlsym found\n",
                imports.openCount, imports.connectCount);
}
