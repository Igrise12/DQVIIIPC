// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/kernel/hle.h"
#include "runtime/iop/kernel/iop_printf.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <cstring>

// loadcore, sysmem, sysclib and stdio as HLE (Docs/milestones/M4-playable.md, session A; the census
// ordinals).
namespace dq8::rt {

namespace {

void charge_bytes(HleCall& c, std::uint64_t bytes) {
    c.charge(bytes * c.exec().cost().iop_hle_per_byte);
}

std::string format(HleCall& c, std::uint32_t fmt, unsigned first_slot) {
    IopPrintfSource src;
    src.word = [&c](unsigned slot) { return c.arg(slot); };
    src.string_at = [&c](std::uint32_t p) { return c.read_string(p, 4096); };
    return iop_format(c.read_string(fmt, 4096), src, first_slot);
}

// ---- loadcore

void flush_dcache(HleCall&) {}

void register_library_entries(HleCall& c) {
    const std::uint32_t table = c.arg(0);
    const std::string module = c.exec().module_at(c.cpu().gpr[31]);
    c.ram_range(table, 4);
    const std::int32_t r = c.exec().libraries().register_guest(c.exec().ram(), table, module);
    c.exec().machine().trace(TraceCategory::iop, module + " registers the export table at " + hex32(table) +
                                                     (r == 0 ? "" : " (refused: " + std::to_string(r) + ")"));
    c.ret(r);
}

void release_library_entries(HleCall& c) {
    c.ret(c.exec().libraries().release_guest(c.arg(0)));
}

// ---- sysmem

void alloc_sys_memory(HleCall& c) {
    const std::uint32_t mode = c.arg(0);
    const std::uint32_t size = c.arg(1);
    const std::uint32_t addr = c.arg(2);
    if (mode > 2) {
        c.fail("allocation mode " + std::to_string(mode) + " (only 0, 1 and 2 are implemented)");
    }
    const std::uint32_t block = size == 0 ? 0 : c.exec().alloc(mode, size, addr);
    c.exec().machine().trace(TraceCategory::iop, "AllocSysMemory(" + std::to_string(mode) + ", " +
                                                     std::to_string(size) + ") = " + hex32(block));
    c.ret(block);
}

void free_sys_memory(HleCall& c) {
    c.ret(c.exec().free(c.arg(0)) ? iop_error::ok : iop_error::error);
}

void kprintf(HleCall& c) {
    const std::string text = format(c, c.arg(0), 1);
    c.exec().print(text);
    c.ret(static_cast<std::uint32_t>(text.size()));
}

// ---- sysclib

// The C locale's character classes as the IOP table encodes them: upper, lower, digit, space, punctuation,
// control, hex digit, blank.
std::uint8_t ctype_flags(std::uint32_t c) {
    constexpr std::uint8_t upper = 0x01;
    constexpr std::uint8_t lower = 0x02;
    constexpr std::uint8_t digit = 0x04;
    constexpr std::uint8_t space = 0x08;
    constexpr std::uint8_t punct = 0x10;
    constexpr std::uint8_t control = 0x20;
    constexpr std::uint8_t hex = 0x40;
    constexpr std::uint8_t blank = 0x80;
    c &= 0xFFu;
    if (c >= 0x80) {
        return 0;
    }
    if (c == ' ') {
        return space | blank;
    }
    if (c < 0x20 || c == 0x7F) {
        return static_cast<std::uint8_t>(control | (c >= '\t' && c <= '\r' ? space : 0));
    }
    if (c >= '0' && c <= '9') {
        return digit;
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<std::uint8_t>(upper | (c <= 'F' ? hex : 0));
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<std::uint8_t>(lower | (c <= 'f' ? hex : 0));
    }
    return punct;
}

void look_ctype_table(HleCall& c) {
    c.ret(std::uint32_t{ctype_flags(c.arg(0))});
}

void memcpy_(HleCall& c) {
    const std::uint32_t dst = c.arg(0);
    const std::uint32_t src = c.arg(1);
    const std::uint32_t n = c.arg(2);
    if (n != 0) {
        const std::uint32_t d = c.ram_range(dst, n);
        const std::uint32_t s = c.ram_range(src, n);
        std::uint8_t* ram = c.exec().ram().data();
        if (d <= s || d >= s + n) {
            std::memmove(ram + d, ram + s, n);
        } else {
            for (std::uint32_t i = 0; i < n; ++i) { // a forward copy over an overlap repeats the pattern
                ram[d + i] = ram[s + i];
            }
        }
    }
    charge_bytes(c, n);
    c.ret(dst);
}

void memset_(HleCall& c) {
    const std::uint32_t dst = c.arg(0);
    const std::uint32_t n = c.arg(2);
    if (n != 0) {
        std::memset(c.exec().ram().data() + c.ram_range(dst, n), static_cast<int>(c.arg(1) & 0xFFu), n);
    }
    charge_bytes(c, n);
    c.ret(dst);
}

void bzero_(HleCall& c) {
    const std::uint32_t n = c.arg(1);
    if (n != 0) {
        std::memset(c.exec().ram().data() + c.ram_range(c.arg(0), n), 0, n);
    }
    charge_bytes(c, n);
}

void strlen_(HleCall& c) {
    const std::uint32_t s = c.arg(0);
    std::uint32_t n = 0;
    while (c.read8(s + n) != 0) {
        ++n;
    }
    charge_bytes(c, n);
    c.ret(n);
}

void strncmp_(HleCall& c) {
    const std::uint32_t a = c.arg(0);
    const std::uint32_t b = c.arg(1);
    const std::uint32_t n = c.arg(2);
    std::int32_t r = 0;
    std::uint32_t i = 0;
    for (; i < n; ++i) {
        const std::uint8_t x = c.read8(a + i);
        const std::uint8_t y = c.read8(b + i);
        if (x != y) {
            r = static_cast<std::int32_t>(x) - static_cast<std::int32_t>(y);
            break;
        }
        if (x == 0) {
            break;
        }
    }
    charge_bytes(c, i);
    c.ret(r);
}

void strtol_(HleCall& c) {
    const std::uint32_t s = c.arg(0);
    const std::uint32_t endptr = c.arg(1);
    auto base = static_cast<std::int32_t>(c.arg(2));
    std::uint32_t p = s;
    auto ch = [&c](std::uint32_t at) { return static_cast<char>(c.read8(at)); };
    while ((ctype_flags(static_cast<std::uint8_t>(ch(p))) & 0x08) != 0) {
        ++p;
    }
    bool negative = false;
    if (ch(p) == '+' || ch(p) == '-') {
        negative = ch(p) == '-';
        ++p;
    }
    if ((base == 0 || base == 16) && ch(p) == '0' && (ch(p + 1) == 'x' || ch(p + 1) == 'X')) {
        const char d = ch(p + 2);
        if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F')) {
            p += 2;
            base = 16;
        }
    }
    if (base == 0) {
        base = ch(p) == '0' ? 8 : 10;
    }
    std::uint64_t value = 0;
    bool any = false;
    const std::uint64_t limit = negative ? std::uint64_t{1} << 31 : (std::uint64_t{1} << 31) - 1;
    if (base >= 2 && base <= 36) {
        for (;; ++p) {
            const char d = ch(p);
            int v = -1;
            if (d >= '0' && d <= '9') {
                v = d - '0';
            } else if (d >= 'a' && d <= 'z') {
                v = d - 'a' + 10;
            } else if (d >= 'A' && d <= 'Z') {
                v = d - 'A' + 10;
            }
            if (v < 0 || v >= base) {
                break;
            }
            any = true;
            value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(v);
            value = std::min(value, limit);
        }
    }
    if (endptr != 0) {
        c.write32(endptr, any ? p : s);
    }
    const std::int64_t r = negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    c.ret(static_cast<std::uint32_t>(static_cast<std::int32_t>(r)));
}

// ---- stdio

void printf_(HleCall& c) {
    const std::string text = format(c, c.arg(0), 1);
    c.exec().print(text);
    c.ret(static_cast<std::uint32_t>(text.size()));
}

constexpr HleFunction loadcore_table[] = {
    {"loadcore", 5, "FlushDcache", flush_dcache},
    {"loadcore", 6, "RegisterLibraryEntries", register_library_entries},
    {"loadcore", 7, "ReleaseLibraryEntries", release_library_entries},
};

constexpr HleFunction sysmem_table[] = {
    {"sysmem", 4, "AllocSysMemory", alloc_sys_memory},
    {"sysmem", 5, "FreeSysMemory", free_sys_memory},
    {"sysmem", 14, "Kprintf", kprintf},
};

constexpr HleFunction sysclib_table[] = {
    {"sysclib", 8, "look_ctype_table", look_ctype_table},
    {"sysclib", 12, "memcpy", memcpy_},
    {"sysclib", 14, "memset", memset_},
    {"sysclib", 17, "bzero", bzero_},
    {"sysclib", 27, "strlen", strlen_},
    {"sysclib", 29, "strncmp", strncmp_},
    {"sysclib", 36, "strtol", strtol_},
};

constexpr HleFunction stdio_table[] = {
    {"stdio", 4, "printf", printf_},
};

} // namespace

std::span<const HleFunction> hle_loadcore() {
    return loadcore_table;
}

std::span<const HleFunction> hle_sysmem() {
    return sysmem_table;
}

std::span<const HleFunction> hle_sysclib() {
    return sysclib_table;
}

std::span<const HleFunction> hle_stdio() {
    return stdio_table;
}

} // namespace dq8::rt
