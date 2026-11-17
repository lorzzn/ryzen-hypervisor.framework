#ifndef RHV_CLIENT_IMAGE_HPP
#define RHV_CLIENT_IMAGE_HPP

#include <stddef.h>
#include <stdint.h>

namespace rhv { namespace compat {

struct Reader {
    void *context;
    bool (*read)(void *, uint64_t, void *, size_t);
};

/* Fixed-width Mach-O records shared by kernel code and host tests. */
struct MachHeader {
    uint32_t magic, cpu, subtype, filetype, commands, commandBytes, flags, reserved;
};
struct Command { uint32_t kind, bytes; };
struct Segment {
    uint32_t kind, bytes;
    char name[16];
    uint64_t address, size, fileOffset, fileSize;
    uint32_t maxProtection, protection, sections, flags;
};
struct Section {
    char name[16], segment[16];
    uint64_t address, size;
    uint32_t offset, alignment, relocations, relocationCount;
    uint32_t flags, indirectStart, stride, reserved;
};
struct Symbol {
    uint32_t string;
    uint8_t type, section;
    uint16_t description;
    uint64_t value;
};
struct SymbolTable {
    uint32_t kind, bytes, offset, count, strings, stringBytes;
};
struct IndirectTable { uint32_t kind, bytes, fields[18]; };

struct Imports {
    uint64_t openSlots[4], connectSlots[4], lookupStub;
    uint32_t openCount, connectCount;
};

inline bool sameName(const char *text, size_t capacity, const char *expected)
{
    size_t i = 0;
    while (i < capacity && expected[i] && text[i] == expected[i]) i++;
    return i < capacity && !expected[i] && !text[i];
}

inline bool contains(uint64_t start, uint64_t size, uint64_t address, uint64_t bytes)
{
    return start <= UINT64_MAX - size && address >= start && address - start <= size &&
           bytes <= size - (address - start);
}

inline bool readAt(const Reader &reader, uint64_t address, void *out, size_t size)
{
    return address <= UINT64_MAX - size && reader.read(reader.context, address, out, size);
}

inline bool findImports(const Reader &reader, uint64_t base, Imports *out)
{
    MachHeader header;
    if (!out || !readAt(reader, base, &header, sizeof(header)) ||
        header.magic != 0xfeedfacf || header.cpu != 0x01000007 || header.filetype != 2 ||
        !header.commands || header.commands > 256 || header.commandBytes > 65536 ||
        header.commandBytes < header.commands * sizeof(Command) ||
        base > UINT64_MAX - sizeof(header) - header.commandBytes) return false;

    struct SelectedSection { Section section; uint64_t segment, size; };
    SelectedSection selected[16];
    unsigned selectedCount = 0;
    Segment text{}, linkedit{};
    SymbolTable symbols{};
    IndirectTable indirect{};
    unsigned textCount = 0, linkeditCount = 0, symbolCount = 0, indirectCount = 0;
    uint64_t cursor = base + sizeof(header);
    const uint64_t end = cursor + header.commandBytes;
    for (uint32_t i = 0; i < header.commands; i++) {
        Command command;
        if (!contains(base + sizeof(header), header.commandBytes, cursor, sizeof(command)) ||
            !readAt(reader, cursor, &command, sizeof(command)) ||
            command.bytes < sizeof(command) || command.bytes % 8 ||
            command.bytes > end - cursor) return false;
        if (command.kind == 0x19) {
            Segment segment;
            if (command.bytes < sizeof(segment) ||
                !readAt(reader, cursor, &segment, sizeof(segment)) ||
                segment.sections > 512 ||
                segment.sections > (command.bytes - sizeof(segment)) / sizeof(Section) ||
                segment.fileSize > segment.size ||
                segment.address > UINT64_MAX - segment.size ||
                segment.fileOffset > UINT64_MAX - segment.fileSize) return false;
            if (sameName(segment.name, sizeof(segment.name), "__TEXT")) {
                text = segment;
                textCount++;
            }
            if (sameName(segment.name, sizeof(segment.name), "__LINKEDIT")) {
                linkedit = segment;
                linkeditCount++;
            }
            for (uint32_t j = 0; j < segment.sections; j++) {
                Section section;
                if (!readAt(reader, cursor + sizeof(segment) + j * sizeof(section),
                            &section, sizeof(section)) ||
                    !contains(segment.address, segment.size, section.address, section.size)) return false;
                uint32_t type = section.flags & 0xff;
                if (type != 6 && type != 7 && type != 8) continue;
                if (selectedCount == sizeof(selected) / sizeof(selected[0])) return false;
                selected[selectedCount++] = {section, segment.address, segment.size};
            }
        } else if (command.kind == 2) {
            if (command.bytes != sizeof(symbols) ||
                !readAt(reader, cursor, &symbols, sizeof(symbols))) return false;
            symbolCount++;
        } else if (command.kind == 0xb) {
            if (command.bytes != sizeof(indirect) ||
                !readAt(reader, cursor, &indirect, sizeof(indirect))) return false;
            indirectCount++;
        }
        cursor += command.bytes;
    }
    if (cursor != end || textCount != 1 || linkeditCount != 1 ||
        symbolCount != 1 || indirectCount != 1 || text.fileOffset != 0 ||
        base < text.address || !symbols.count || symbols.count > 1000000 ||
        !symbols.stringBytes || symbols.stringBytes > 32 * 1024 * 1024 ||
        !indirect.fields[13] || indirect.fields[13] > 1000000) return false;
    uint64_t slide = base - text.address;
    if (linkedit.address > UINT64_MAX - slide ||
        linkedit.size > UINT64_MAX - linkedit.address - slide ||
        linkedit.address + slide < linkedit.fileOffset) return false;
    uint64_t fileBase = linkedit.address + slide - linkedit.fileOffset;
    if (!contains(linkedit.fileOffset, linkedit.fileSize, symbols.offset,
                  (uint64_t)symbols.count * sizeof(Symbol)) ||
        !contains(linkedit.fileOffset, linkedit.fileSize, symbols.strings, symbols.stringBytes) ||
        !contains(linkedit.fileOffset, linkedit.fileSize, indirect.fields[12],
                  (uint64_t)indirect.fields[13] * sizeof(uint32_t))) return false;

    Imports result{};
    unsigned lookupCount = 0;
    for (unsigned i = 0; i < selectedCount; i++) {
        const Section &section = selected[i].section;
        const bool stubs = (section.flags & 0xff) == 8;
        uint64_t stride = stubs ? section.stride : sizeof(uint64_t);
        if (selected[i].segment > UINT64_MAX - slide ||
            selected[i].size > UINT64_MAX - selected[i].segment - slide ||
            !stride || (stubs && (stride < 6 || stride > 32)) ||
            section.size % stride || section.size / stride > 4096 ||
            section.address > UINT64_MAX - slide ||
            !contains(0, indirect.fields[13], section.indirectStart, section.size / stride)) return false;
        for (uint64_t j = 0; j < section.size / stride; j++) {
            uint32_t index;
            uint64_t indirectOffset = indirect.fields[12] +
                                      (section.indirectStart + j) * sizeof(index);
            if (!readAt(reader, fileBase + indirectOffset, &index, sizeof(index))) return false;
            if (index & 0xc0000000) continue;
            if (index >= symbols.count) return false;
            Symbol symbol;
            if (!readAt(reader, fileBase + symbols.offset + index * sizeof(symbol),
                        &symbol, sizeof(symbol)) || symbol.string >= symbols.stringBytes) return false;
            if ((symbol.type & 0x0e) != 0) continue;
            char name[48] = {0};
            size_t length = symbols.stringBytes - symbol.string;
            if (length > sizeof(name)) length = sizeof(name);
            if (!readAt(reader, fileBase + symbols.strings + symbol.string, name, length)) return false;
            uint64_t address = section.address + slide + j * stride;
            if (stubs) {
                if (sameName(name, sizeof(name), "_dlsym")) {
                    result.lookupStub = address;
                    lookupCount++;
                }
            } else {
                if (address % sizeof(uint64_t)) return false;
                if (sameName(name, sizeof(name), "_open")) {
                    if (result.openCount == 4) return false;
                    result.openSlots[result.openCount++] = address;
                } else if (sameName(name, sizeof(name), "_connect")) {
                    if (result.connectCount == 4) return false;
                    result.connectSlots[result.connectCount++] = address;
                }
            }
        }
    }
    if (!result.openCount || !result.connectCount || lookupCount != 1) return false;
    *out = result;
    return true;
}

} }
#endif
