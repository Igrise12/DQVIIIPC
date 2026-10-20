// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// dq8_e2e_fixtures <out-dir>: writes the hand-assembled code units the end-to-end tests recompile, build and
// run. Every instruction is assembled here (support/mips_asm.h); every address, size and name is ours.
// Nothing is read from a disc.
//
// For each fixture, <out-dir>/<fixture>/ gets units/<unit>.elf (what ps2_recomp reads) and, in the map format
// the Ghidra exporter writes (tools/ghidra/ExportFunctionMap.java), maps/<unit>.csv, <unit>.jump_tables.toml
// and <unit>.stats.toml. Overlay units are also written as an MWo3 container, units/<unit>.mwo3, and their
// ELF is made from it by the pipeline's own make_overlay_unit_elf.
//
// Fixtures; each program sets a0 = 0x12345678, reaches a helper that sets v0 = a0 + 1 and returns, then puts
// 0x3C in v1 and executes syscall:
//   call     main unit only; the helper is reached by jal (dispatched, resumed at jal + 8)
//   bgezal   main unit only; the helper is reached by bgezal a0 (taken), whose return address is listed in
//            link_return_entries because the recompiler makes no resume entry for it
//   overlay  the helper lives in a second unit, an overlay at a synthetic window; main reaches it by jal

#include "dq8/core/elf_writer.h"
#include "dq8/core/error.h"
#include "dq8/core/mwo3.h"
#include "dq8/core/overlay_elf.h"
#include "support/mips_asm.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace mips = dq8::test::mips;
using mips::Reg;

namespace {

// Synthetic layout, ours: a main unit at the conventional user-program base and an overlay window well
// away from it, so a test that passes cannot depend on any game's addresses.
constexpr std::uint32_t main_base = 0x00100000;
constexpr std::uint32_t window_base = 0x00300000;
constexpr std::uint32_t window_end = 0x00310000;
constexpr std::uint32_t overlay_id = 1;

constexpr std::uint32_t a0_hi = 0x1234;
constexpr std::uint32_t a0_lo = 0x5678;
constexpr std::int32_t syscall_number = 0x3C;

struct Function {
    std::string name;
    std::uint32_t start = 0;
    std::uint32_t end = 0; // exclusive
};

struct Unit {
    std::string name;
    std::vector<Function> functions;
    std::vector<std::uint32_t> link_return_entries;
};

void write_bytes(const fs::path& file, std::span<const std::uint8_t> bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw dq8::core::Error(dq8::core::ErrorCode::io_write, "cannot write " + file.string());
    }
}

void write_text(const fs::path& file, const std::string& text) {
    write_bytes(file, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string hex32(std::uint32_t value) {
    char buf[11];
    std::snprintf(buf, sizeof buf, "0x%08X", value);
    return buf;
}

void write_maps(const fs::path& dir, const Unit& unit) {
    std::string csv = "Name,Start,End,Size\n";
    for (const Function& f : unit.functions) {
        csv +=
            f.name + "," + hex32(f.start) + "," + hex32(f.end) + "," + std::to_string(f.end - f.start) + "\n";
    }
    write_text(dir / (unit.name + ".csv"), csv);
    write_text(dir / (unit.name + ".jump_tables.toml"), "# Synthetic fixture: no computed jumps.\n");

    std::string stats = "# Synthetic fixture.\nfunctions = " + std::to_string(unit.functions.size()) +
                        "\nlink_return_entries = [";
    for (std::size_t i = 0; i < unit.link_return_entries.size(); ++i) {
        stats += (i != 0 ? ", " : "") + hex32(unit.link_return_entries[i]);
    }
    stats += "]\n";
    write_text(dir / (unit.name + ".stats.toml"), stats);
}

// A main-unit ELF: one executable .text at the program's base, entry at its first word.
std::vector<std::uint8_t> main_elf(const mips::Program& program) {
    dq8::core::ElfWriter writer;
    writer.set_entry(program.base());
    dq8::core::ElfSection text;
    text.name = ".text";
    text.flags = dq8::core::elf_shf_alloc | dq8::core::elf_shf_execinstr;
    text.address = program.base();
    text.align = 16;
    text.data = program.bytes();
    writer.add_section(std::move(text));
    return writer.build();
}

// Emits the program's start: a0 = 0x12345678. Returns nothing; the caller emits the transfer to the helper.
void emit_prologue(mips::Program& p) {
    p.emit(mips::lui(Reg::a0, a0_hi));
    p.emit(mips::ori(Reg::a0, Reg::a0, a0_lo));
}

// Emits v1 = 0x3C; syscall; then a return the run never reaches (the syscall stops it).
void emit_epilogue(mips::Program& p) {
    p.emit(mips::addiu(Reg::v1, Reg::zero, syscall_number));
    p.emit(mips::syscall());
    p.emit(mips::jr(Reg::ra));
    p.emit(mips::nop());
}

// helper: v0 = a0 + 1; return.
void emit_helper(mips::Program& p) {
    p.emit(mips::addiu(Reg::v0, Reg::a0, 1));
    p.emit(mips::jr(Reg::ra));
    p.emit(mips::nop());
}

void write_call_fixture(const fs::path& dir) {
    mips::Program p(main_base);
    emit_prologue(p);
    const std::uint32_t call = p.emit(mips::nop()); // jal e2e_helper, patched below
    p.emit(mips::nop());
    emit_epilogue(p);
    const std::uint32_t helper = p.here();
    emit_helper(p);
    p.patch(call, mips::jal(helper));

    write_bytes(dir / "units/main.elf", main_elf(p));
    write_maps(dir / "maps",
               Unit{"main", {{"e2e_start", main_base, helper}, {"e2e_helper", helper, p.here()}}, {}});
}

void write_bgezal_fixture(const fs::path& dir) {
    mips::Program p(main_base);
    emit_prologue(p);
    const std::uint32_t link = p.emit(mips::nop()); // bgezal a0, e2e_helper, patched below
    p.emit(mips::nop());
    emit_epilogue(p);
    const std::uint32_t helper = p.here();
    emit_helper(p);
    p.patch(link, mips::bgezal(Reg::a0, mips::branch_offset(link, helper)));

    write_bytes(dir / "units/main.elf", main_elf(p));
    write_maps(
        dir / "maps",
        Unit{"main", {{"e2e_start", main_base, helper}, {"e2e_helper", helper, p.here()}}, {link + 8}});
}

// An MWo3 container around `text` for the synthetic window: the header (mwo3.h layout), the code, and a
// 16-byte data range holding an empty constructor table.
std::vector<std::uint8_t> mwo3_container(std::span<const std::uint8_t> text) {
    constexpr std::uint32_t data_size = 16;
    constexpr std::uint32_t bss_size = 16;
    const auto text_size = static_cast<std::uint32_t>(text.size());
    const std::uint32_t data_start =
        window_base + static_cast<std::uint32_t>(dq8::core::mwo3_header_size) + text_size;
    const std::uint32_t words[8] = {dq8::core::mwo3_magic_word,
                                    overlay_id,
                                    window_base,
                                    text_size,
                                    data_size,
                                    bss_size,
                                    data_start,
                                    data_start};
    std::vector<std::uint8_t> out(dq8::core::mwo3_header_size, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t b = 0; b < 4; ++b) {
            out[4 * i + b] = static_cast<std::uint8_t>(words[i] >> (8 * b));
        }
    }
    const std::string name = "e2e-overlay";
    std::copy(name.begin(), name.end(), out.begin() + 0x20);
    out.insert(out.end(), text.begin(), text.end());
    out.resize(out.size() + data_size, 0);
    return out;
}

void write_overlay_fixture(const fs::path& dir) {
    // The overlay: its code starts after the 64-byte container header, as the game's loader maps it.
    mips::Program ovl(window_base + static_cast<std::uint32_t>(dq8::core::mwo3_header_size));
    const std::uint32_t helper = ovl.here();
    emit_helper(ovl);
    ovl.emit(mips::nop()); // pad the text to 16 bytes
    const std::vector<std::uint8_t> container = mwo3_container(ovl.bytes());
    write_bytes(dir / "units/ovl.mwo3", container);
    const dq8::core::OverlayUnitElf unit =
        dq8::core::make_overlay_unit_elf(container, 0, dq8::core::Mwo3Window{window_base, window_end});
    write_bytes(dir / "units/ovl.elf", unit.elf);
    write_maps(dir / "maps", Unit{"ovl", {{"e2e_ovl_helper", helper, helper + 12}}, {}});

    mips::Program p(main_base);
    emit_prologue(p);
    p.emit(mips::jal(helper));
    p.emit(mips::nop());
    emit_epilogue(p);
    write_bytes(dir / "units/main.elf", main_elf(p));
    write_maps(dir / "maps", Unit{"main", {{"e2e_start", main_base, p.here()}}, {}});
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: dq8_e2e_fixtures <out-dir>\n");
        return 2;
    }
    try {
        const fs::path out = argv[1];
        write_call_fixture(out / "call");
        write_bgezal_fixture(out / "bgezal");
        write_overlay_fixture(out / "overlay");
        std::printf("fixtures written to %s (window %s..%s, overlay id %u)\n", out.string().c_str(),
                    hex32(window_base).c_str(), hex32(window_end).c_str(), overlay_id);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8_e2e_fixtures: %s\n", e.what());
        return 1;
    }
}
