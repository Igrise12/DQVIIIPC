// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// IRX modules (Docs/milestones/M4-playable.md, session A): IOP relocatable executables. An IRX is a 32-bit
// little-endian MIPS ELF of type 0xFF80 whose first program header (type 0x70000080) points at the module's
// .iopmod record (module info pointer, entry, gp, text/data/bss sizes, version, name) and whose PT_LOAD
// segment is linked at address 0. REL sections relocate it to its load address: R_MIPS_32, R_MIPS_26 and
// HI16/LO16 pairs, all against the image base (symbol index 0). Anything else is refused with the reason, so
// the loader stops instead of running a half-relocated module.
//
// Import tables live in the text: a magic word, a link word (0 in the file), the library version, the
// 8-byte library name, then one 8-byte stub per function (`jr $ra` and `addiu $zero, $zero, ordinal`) and two
// zero words. Export tables have the same header with another magic word and a zero-terminated list of
// function addresses; a module registers its own at start (loadcore RegisterLibraryEntries).
namespace dq8::rt {

class IopMemory;

class IrxError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::uint16_t irx_elf_type = 0xFF80;
inline constexpr std::uint32_t irx_iopmod_type = 0x70000080u;
inline constexpr std::uint32_t irx_import_magic = 0x41E00000u;
inline constexpr std::uint32_t irx_export_magic = 0x41C00000u;
inline constexpr std::uint32_t irx_stub_jr_ra = 0x03E00008u;
inline constexpr std::uint32_t irx_stub_ordinal = 0x24000000u; // addiu $zero, $zero, ordinal
inline constexpr std::uint32_t irx_table_header = 20;          // magic, link, version, name[8]

struct IrxRelocation {
    std::uint32_t offset = 0; // image-relative
    std::uint8_t type = 0;
};

struct IrxModule {
    std::string name; // from .iopmod
    std::uint16_t version = 0;
    std::uint32_t entry = 0; // image-relative
    std::uint32_t gp = 0;    // image-relative
    std::uint32_t text_size = 0;
    std::uint32_t data_size = 0;
    std::uint32_t bss_size = 0;
    std::uint32_t mem_size = 0;        // the loaded image, bss included
    std::vector<std::uint8_t> bytes;   // the file part of the image (text and data)
    std::vector<IrxRelocation> relocs; // in section and entry order
};

struct IrxImportStub {
    std::uint32_t address = 0; // the stub's first word, in IOP RAM
    std::uint16_t ordinal = 0;
};

struct IrxImportTable {
    std::uint32_t address = 0;
    std::string library;
    std::uint16_t version = 0;
    std::vector<IrxImportStub> stubs;
};

// Parses and checks an IRX file; IrxError says what is wrong.
IrxModule parse_irx(std::span<const std::uint8_t> file);
// The image relocated to `base`: text, data, then zeroed bss (mem_size bytes).
std::vector<std::uint8_t> relocate_irx(const IrxModule& module, std::uint32_t base);
// The import tables in the text of a module loaded at `base`.
std::vector<IrxImportTable> find_irx_imports(const IopMemory& ram, std::uint32_t base,
                                             std::uint32_t text_size);
// An 8-byte library name field as a string (up to its first NUL).
std::string irx_library_name(const IopMemory& ram, std::uint32_t address);

} // namespace dq8::rt
