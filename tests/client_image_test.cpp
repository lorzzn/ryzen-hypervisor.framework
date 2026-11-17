#include "../include/rhv_client_image.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <vector>

using rhv::compat::Imports;
using rhv::compat::Reader;

static constexpr uint64_t base = 0x180000000;
static constexpr uint64_t preferred = 0x100000000;

struct Image {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(0x3000);
    size_t command = sizeof(mach_header_64);
    size_t symbol_offset = 0x2000;
    size_t string_offset = 0x2100;
    size_t indirect_offset = 0x2200;

    template<typename T> void put(size_t offset, const T &value)
    {
        assert(offset + sizeof(value) <= bytes.size());
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    void segment(const char *name, uint64_t offset, const char *section_name,
                 uint32_t type, uint64_t section_offset, uint64_t section_size,
                 uint32_t first, uint32_t stride)
    {
        segment_command_64 segment{};
        segment.cmd = LC_SEGMENT_64;
        segment.cmdsize = sizeof(segment) + (section_name ? sizeof(section_64) : 0);
        std::strncpy(segment.segname, name, sizeof(segment.segname));
        segment.vmaddr = preferred + offset;
        segment.vmsize = 0x1000;
        segment.fileoff = offset;
        segment.filesize = 0x1000;
        segment.nsects = section_name ? 1 : 0;
        put(command, segment);
        if (section_name) {
            section_64 section{};
            std::strncpy(section.sectname, section_name, sizeof(section.sectname));
            std::strncpy(section.segname, name, sizeof(section.segname));
            section.addr = preferred + section_offset;
            section.size = section_size;
            section.flags = type;
            section.reserved1 = first;
            section.reserved2 = stride;
            put(command + sizeof(segment), section);
        }
        command += segment.cmdsize;
    }

    Image()
    {
        segment("__TEXT", 0, "__stubs", S_SYMBOL_STUBS, 0x800, 6, 2, 6);
        segment("__DATA", 0x1000, "__la_symbol_ptr", S_LAZY_SYMBOL_POINTERS,
                0x1000, 16, 0, 0);
        segment("__LINKEDIT", 0x2000, nullptr, 0, 0, 0, 0, 0);
        symtab_command symbols{};
        symbols.cmd = LC_SYMTAB;
        symbols.cmdsize = sizeof(symbols);
        symbols.symoff = (uint32_t)symbol_offset;
        symbols.nsyms = 3;
        symbols.stroff = (uint32_t)string_offset;
        const char strings[] = "\0_open\0_connect\0_dlsym\0";
        symbols.strsize = sizeof(strings);
        put(command, symbols);
        command += sizeof(symbols);
        std::memcpy(bytes.data() + string_offset, strings, sizeof(strings));
        const uint32_t names[] = {1, 7, 16};
        for (unsigned i = 0; i < 3; i++) {
            nlist_64 symbol{};
            symbol.n_un.n_strx = names[i];
            symbol.n_type = N_UNDF | N_EXT;
            put(symbol_offset + i * sizeof(symbol), symbol);
            put(indirect_offset + i * sizeof(uint32_t), i);
        }
        dysymtab_command indirect{};
        indirect.cmd = LC_DYSYMTAB;
        indirect.cmdsize = sizeof(indirect);
        indirect.indirectsymoff = (uint32_t)indirect_offset;
        indirect.nindirectsyms = 3;
        put(command, indirect);
        command += sizeof(indirect);
        mach_header_64 header{};
        header.magic = MH_MAGIC_64;
        header.cputype = CPU_TYPE_X86_64;
        header.filetype = MH_EXECUTE;
        header.ncmds = 5;
        header.sizeofcmds = (uint32_t)(command - sizeof(header));
        put(0, header);
    }

    static bool read(void *context, uint64_t address, void *out, size_t size)
    {
        auto *image = static_cast<Image *>(context);
        if (address < base || address - base > image->bytes.size() ||
            size > image->bytes.size() - (address - base)) return false;
        std::memcpy(out, image->bytes.data() + address - base, size);
        return true;
    }

    bool parse(Imports *imports)
    {
        return rhv::compat::findImports(Reader{this, read}, base, imports);
    }
};

int main()
{
    static_assert(sizeof(rhv::compat::MachHeader) == sizeof(mach_header_64));
    static_assert(sizeof(rhv::compat::Segment) == sizeof(segment_command_64));
    static_assert(sizeof(rhv::compat::Section) == sizeof(section_64));
    static_assert(sizeof(rhv::compat::Symbol) == sizeof(nlist_64));
    Imports imports{};
    Image valid;
    assert(valid.parse(&imports));
    assert(imports.openCount == 1 && imports.connectCount == 1);
    assert(imports.openSlots[0] == base + 0x1000);
    assert(imports.connectSlots[0] == base + 0x1008);
    assert(imports.lookupStub == base + 0x800);

    Image bad_magic;
    bad_magic.put<uint32_t>(0, 0);
    assert(!bad_magic.parse(&imports));
    Image bad_command;
    bad_command.put<uint32_t>(sizeof(mach_header_64) + 4, 0);
    assert(!bad_command.parse(&imports));
    Image too_many;
    too_many.put<uint32_t>(offsetof(mach_header_64, ncmds), 10000);
    assert(!too_many.parse(&imports));
    Image bad_symbol;
    bad_symbol.put<uint32_t>(bad_symbol.indirect_offset, 300);
    assert(!bad_symbol.parse(&imports));
    Image bad_string;
    bad_string.put<uint32_t>(bad_string.symbol_offset, 0xfffffff0);
    assert(!bad_string.parse(&imports));
    Image bad_section;
    bad_section.put<uint64_t>(sizeof(mach_header_64) + sizeof(segment_command_64) +
                              offsetof(section_64, addr), UINT64_MAX - 16);
    assert(!bad_section.parse(&imports));
    Image missing_import;
    missing_import.bytes[missing_import.string_offset + 1] = 'x';
    assert(!missing_import.parse(&imports));
    Image outside_linkedit;
    size_t symtab = sizeof(mach_header_64) + 2 * (sizeof(segment_command_64) + sizeof(section_64)) +
                    sizeof(segment_command_64);
    outside_linkedit.put<uint32_t>(symtab + offsetof(symtab_command, stroff), 0x1000);
    assert(!outside_linkedit.parse(&imports));
    Image overflowing_segment;
    overflowing_segment.put<uint64_t>(sizeof(mach_header_64) +
                                      offsetof(segment_command_64, vmsize), UINT64_MAX);
    assert(!overflowing_segment.parse(&imports));
    Image overflowing_slide;
    size_t data_segment = sizeof(mach_header_64) + sizeof(segment_command_64) + sizeof(section_64);
    overflowing_slide.put<uint64_t>(data_segment + offsetof(segment_command_64, vmsize),
                                   UINT64_MAX - preferred - 0x1000);
    assert(!overflowing_slide.parse(&imports));
    std::puts("client_image_test: 11 cases passed");
    return 0;
}
