// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/game_module.h"

#include <cstdint>

// A hand-written code unit in the shape PS2Recomp's emitter produces (function_emitter.cpp,
// control_flow_emitter.cpp and function_table_emitter.cpp at eb8688b): one C++ function per guest function
// with a resume `switch (ctx->pc)`, every `jal` through dispatchGuestBranch, `jr $ra` as `ctx->pc = ra;
// return;`, syscalls through handleSyscall, and a dense table filled by a static initialiser. The guest
// program is synthetic (our own addresses and instructions, listed in contract_unit.cpp); nothing is taken
// from a game.
//
// It is compiled into the unit tests (test_ee_runtime_contract.cpp) and, with contract_unit_module.cpp, into
// a shared library that dq8-game --elf loads like any game module.
namespace contract {

inline constexpr std::uint32_t table_base = 0x00200000u;
inline constexpr std::uint32_t table_end = 0x00200600u;
inline constexpr char unit_name[] = "contract";
inline constexpr char source_sha1[] = "0123456789abcdef0123456789abcdef01234567";

// Entry points of the test programs (see contract_unit.cpp for each one's instructions).
inline constexpr std::uint32_t entry = 0x00200000u;            // jal helper; v1 = 0x3C; syscall
inline constexpr std::uint32_t unwind_caller = 0x00200040u;    // jal to a function that jumps elsewhere
inline constexpr std::uint32_t unwind_resume = 0x00200048u;    // the jal+8 resume point of unwind_caller
inline constexpr std::uint32_t ends = 0x00200060u;             // v0 = 7; jr ra (ra = 0 ends the program)
inline constexpr std::uint32_t calls_flush = 0x00200090u;      // jal to the FlushCache kernel-call stub
inline constexpr std::uint32_t stub_memcpy = 0x002000C0u;      // the memcpy library stub
inline constexpr std::uint32_t calls_missing = 0x002000E0u;    // jal to an address with an empty slot
inline constexpr std::uint32_t missing_target = 0x002001F0u;   // the empty slot
inline constexpr std::uint32_t scratchpad_store = 0x00200160u; // sw/lw through a scratchpad literal
inline constexpr std::uint32_t unhandled = 0x002001A0u;        // an instruction the recompiler did not handle
inline constexpr std::uint32_t recurse = 0x002001C0u;          // calls itself forever

// M3 programs (machine and kernel): they run on a dq8::rt::Machine, use kernel calls by number, and keep
// their data at `data`. Each saves its $ra in $s2 around calls and returns through it.
inline constexpr std::uint32_t data = 0x00280000u;         // parameter blocks and result words
inline constexpr std::uint32_t worker_stack = 0x00290000u; // the second thread's stack (0x4000 bytes)
inline constexpr std::uint32_t two_threads = 0x00200200u;  // CreateSema, CreateThread, StartThread, WaitSema
inline constexpr std::uint32_t worker = 0x00200260u;       // marks data+0x84, SignalSema(a0), returns (exits)
inline constexpr std::uint32_t vblank_sema = 0x002002A0u;  // AddIntcHandler(2), EnableIntc(2), WaitSema
inline constexpr std::uint32_t vblank_handler = 0x00200300u; // counts at data+0x88, iSignalSema(a1)
inline constexpr std::uint32_t backedge_irq = 0x00200320u; // AddIntcHandler(2), EnableIntc(2), jal spin_loop
inline constexpr std::uint32_t backedge_resume = 0x00200358u;
inline constexpr std::uint32_t spin_loop = 0x00200380u;    // polls data+0x90 on a back-edge until non-zero
inline constexpr std::uint32_t flag_handler = 0x002003C0u; // stores 1 at data+0x90
inline constexpr std::uint32_t self_loop_caller =
    0x002003E0u; // AddIntcHandler(9), EnableIntc(9), jal self_loop
inline constexpr std::uint32_t self_loop_resume = 0x00200410u;
inline constexpr std::uint32_t self_loop = 0x00200420u; // $s0 += 1 with the back-edge to its own entry
inline constexpr std::uint32_t self_loop_iterations = 2000u;
inline constexpr std::uint32_t jr_self_caller = 0x00200460u; // jal jr_self
inline constexpr std::uint32_t jr_self_resume = 0x00200470u;
inline constexpr std::uint32_t jr_self = 0x002004A0u;       // $s0 += 1, then `jr` to its own entry until 3
inline constexpr std::uint32_t count_handler = 0x002004E0u; // counts at data+0x94

// Host-side counts of C++ entries into the functions above (a resume through the run loop counts again,
// which is how the tests see a return cascade).
struct Counters {
    unsigned spin_loop_entries = 0;
    unsigned self_loop_entries = 0;
    unsigned jr_self_entries = 0;
};
Counters& counters();

} // namespace contract

// The unit's descriptor, as the module template's entry file would fill it.
const dq8_game_module_desc* contract_unit_descriptor();
