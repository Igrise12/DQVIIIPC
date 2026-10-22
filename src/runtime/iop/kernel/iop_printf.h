// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// Our own printf formatter for the IOP's printf and Kprintf (Docs/milestones/M4-playable.md, session A): the
// C conversions an IOP module can use (d i u o x X c s p %, the flags - + space # 0, width and precision with
// *, the length modifiers hh h l ll j z t). Arguments are the caller's 32-bit argument slots from
// `first_slot` on (slot 0 is $a0; a 64-bit argument takes an even-aligned pair of slots, as the o32
// convention passes it); strings come through `string_at`. A conversion it does not know is copied to the
// output as written.
namespace dq8::rt {

struct IopPrintfSource {
    std::function<std::uint32_t(unsigned slot)> word;            // argument slot `slot`
    std::function<std::string(std::uint32_t address)> string_at; // a NUL-terminated string in IOP RAM
};

std::string iop_format(std::string_view format, const IopPrintfSource& source, unsigned first_slot);

} // namespace dq8::rt
