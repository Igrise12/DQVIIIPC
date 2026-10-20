// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/ee/guest_memory.h"

#include <cstdint>
#include <span>
#include <vector>

// Places an executable's PT_LOAD segments in guest memory, the way the EE kernel's loader does and the way
// PS2Recomp's PS2Runtime::loadELF does: filesz bytes copied from the file, memsz - filesz bytes zeroed.
// Segments with no file bytes (a boot ELF's overlay-window reservations, for example) are zeroed in full. The
// code bytes stay in guest memory because the game may read them as data; native code is what executes
// (architecture §4.1).
namespace dq8::rt {

inline constexpr std::uint16_t elf_type_exec = 2; // ET_EXEC

struct LoadedSegment {
    std::uint32_t vaddr = 0;
    std::uint32_t filesz = 0;
    std::uint32_t memsz = 0;
    std::uint32_t flags = 0;
};

struct LoadedElf {
    std::uint32_t entry = 0;
    std::vector<LoadedSegment> segments; // the PT_LOAD segments placed, in program-header order
};

// `file` is the whole ELF file. Throws core::Error(elf_format) when the file is not a little-endian ELF32
// ET_EXEC for EM_MIPS, when a segment's bytes lie outside the file, or when a segment does not fit in RAM
// (one mirror, no wrap) or in the scratchpad. Segments with memsz 0 are skipped.
LoadedElf load_elf(std::span<const std::uint8_t> file, GuestMemory& memory);

} // namespace dq8::rt
