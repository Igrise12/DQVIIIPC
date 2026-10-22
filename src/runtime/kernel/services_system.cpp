// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: selected functions of
// ps2xRuntime/src/lib/Kernel/Syscalls/System.cpp and the OSD configuration packing of Helpers/Runtime.h
// (SetupThread, SetupHeap, EndOfHeap, GetMemorySize, the OSD configuration calls, SetSyscall, Copy,
// GetEntryAddress, FindAddress), re-signed to KernelCallContext. What changed: the heap ends at the main
// thread's stack bottom when SetupHeap gets -1 (Docs/milestones/M3-boots.md, "Boot path"); the OSD defaults
// use no host timezone; Copy and SetSyscall refuse the HLE kernel's reserved RAM; the exit-path calls stop
// the run. The register effects are to be verified against PCSX2 state during bring-up.
#include "dq8/abi/game_module.h"
#include "dq8/abi/ps2recomp/ps2_address.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/ee_services.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_calls.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dq8::rt {

namespace {

using detail::guest_block_ok;
using detail::read_u32;
using detail::write_u32;

// ---- ResetEE's units and the DMA channels whose STR bit says they are working.
constexpr std::uint32_t chcr_str = 1u << 8;
constexpr std::uint32_t reset_ee_units = 0x7Fu;
struct ResetUnit {
    std::uint32_t bit;
    std::string_view name;
    std::uint32_t channels; // bit n: DMA channel n
};
constexpr std::array<ResetUnit, 7> reset_units{{
    {1u << 0, "DMAC", 0x3FFu},
    {1u << 1, "VIF0", 1u << 0},
    {1u << 2, "VIF1", 1u << 1},
    {1u << 3, "GIF", 1u << 2},
    {1u << 4, "VU0", 0},
    {1u << 5, "VU1", 0},
    {1u << 6, "IPU", (1u << 3) | (1u << 4)},
}};
// D0_CHCR..D9_CHCR.
constexpr std::uint32_t dmac_channel_base(std::uint32_t channel) {
    constexpr std::uint32_t bases[10] = {0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                                         0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u};
    return bases[channel];
}

// Ends the run at a kernel call the HLE cannot honour. (The message is built by the caller in its own
// statement: temporaries of the stopping statement would never be destroyed, since the fiber is abandoned.)
[[noreturn]] void stop_unsupported_call(KernelCallContext& c, std::string message) {
    StopInfo s;
    s.reason = StopReason::unsupported;
    s.pc = c.ctx().pc;
    s.message = std::move(message);
    c.machine().stop(std::move(s));
}

// ---- OSD configuration words (packing from upstream's Helpers/Runtime.h).

std::uint32_t pack_osd_config(std::uint32_t spdif, std::uint32_t screen, std::uint32_t video,
                              std::uint32_t japanese, std::uint32_t ps1drv, std::uint32_t version,
                              std::uint32_t language, std::int32_t timezone) {
    std::uint32_t raw = 0;
    raw |= (spdif & 0x1u) << 0;
    raw |= (screen & 0x3u) << 1;
    raw |= (video & 0x1u) << 3;
    raw |= (japanese & 0x1u) << 4;
    raw |= (ps1drv & 0xFFu) << 5;
    raw |= (version & 0x7u) << 13;
    raw |= (language & 0x1Fu) << 16;
    raw |= (static_cast<std::uint32_t>(timezone) & 0x7FFu) << 21;
    return raw;
}

std::uint32_t pack_osd_config2(std::uint32_t format, std::uint32_t daylight, std::uint32_t time_format,
                               std::uint32_t date_format, std::uint32_t version, std::uint32_t language) {
    const std::uint32_t flags =
        ((daylight & 0x1u) << 4) | ((time_format & 0x1u) << 5) | ((date_format & 0x3u) << 6);
    return (format & 0xFFu) | ((flags & 0xFFu) << 8) | ((version & 0xFFu) << 16) | ((language & 0xFFu) << 24);
}

std::uint32_t sanitize_osd_config(std::uint32_t raw) {
    std::uint32_t screen = (raw >> 1) & 0x3u;
    if (screen > 2) {
        screen = 0;
    }
    std::uint32_t version = (raw >> 13) & 0x7u;
    if (version > 2) {
        version = 1;
    }
    std::int32_t tz = static_cast<std::int32_t>((raw >> 21) & 0x7FFu);
    if ((tz & 0x400) != 0) {
        tz -= 0x800; // sign-extend the 11-bit field
    }
    return pack_osd_config(raw & 0x1u, screen, (raw >> 3) & 0x1u, (raw >> 4) & 0x1u, (raw >> 5) & 0xFFu,
                           version, (raw >> 16) & 0x1Fu, tz);
}

std::uint32_t sanitize_osd_config2(std::uint32_t raw) {
    const std::uint32_t flags = (raw >> 8) & 0xFFu;
    std::uint32_t date_format = (flags >> 6) & 0x3u;
    if (date_format > 2) {
        date_format = 0;
    }
    std::uint32_t version = (raw >> 16) & 0xFFu;
    if (version > 2) {
        version = 1;
    }
    return pack_osd_config2(raw & 0xFFu, (flags >> 4) & 0x1u, (flags >> 5) & 0x1u, date_format, version,
                            (raw >> 24) & 0xFFu);
}

// Config2 as GetOsdConfigParam2 reports it: its own low half, ConfigParam's version and language above.
std::uint32_t readable_config2(const KernelSystemState& s) {
    std::uint32_t raw = s.osd_config2 & 0x0000FFFFu;
    raw |= ((s.osd_config >> 13) & 0x7u) << 16;
    raw |= ((s.osd_config >> 16) & 0x1Fu) << 24;
    return sanitize_osd_config2(raw);
}

std::uint32_t without_kseg(std::uint32_t address) noexcept {
    return address >= 0x80000000u && address < 0xC0000000u ? address & 0x1FFFFFFFu : address;
}

// The guest RAM address of syscall-table entry `number` (signed), or nullopt outside the mirrored range.
std::optional<std::uint32_t> table_entry(std::uint32_t number) noexcept {
    const std::int64_t address =
        std::int64_t{kabi::syscall_table_physical} + std::int64_t{static_cast<std::int32_t>(number)} * 4;
    if (address < 0 || address + 4 > std::int64_t{kabi::syscall_mirror_limit}) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(address);
}

std::string read_guest_string(GuestMemory& mem, std::uint32_t address, std::size_t limit) {
    std::string out;
    if (address == 0) {
        return out;
    }
    for (std::size_t i = 0; i < limit && guest_block_ok(address + static_cast<std::uint32_t>(i), 1, 1); ++i) {
        const auto ch = static_cast<char>(mem.load8(address + static_cast<std::uint32_t>(i)));
        if (ch == '\0') {
            break;
        }
        out += (ch >= 0x20 && ch < 0x7F) ? ch : '?';
    }
    return out;
}

[[noreturn]] void stop_exit_path(KernelCallContext& c, std::string what) {
    StopInfo s;
    s.reason = StopReason::program_end;
    s.pc = c.ctx().pc;
    s.has_number = c.has_number();
    s.number = static_cast<std::uint32_t>(c.number());
    s.call_name = std::string(c.called_as());
    s.call_kind = DQ8_CALL_SYSCALL;
    for (unsigned i = 0; i < 4; ++i) {
        s.args[i] = c.arg(i);
    }
    s.message = std::move(what);
    c.machine().stop(std::move(s));
    std::abort();
}

} // namespace

void reset_system_state(KernelSystemState& state) noexcept {
    state.heap_start = 0;
    state.heap_end = 0;
    // SPDIF off, 4:3, RGB, a non-Japanese console, OSD version 1, English, timezone offset 0.
    state.osd_config = pack_osd_config(1, 0, 0, 1, 0, 1, 1, 0);
    state.osd_config2 = pack_osd_config2(0, 0, 0, 0, 1, 1);
}

void register_system_services(KernelServices& services, EeKernel& kernel) {
    const auto add = [&services](std::string name, KernelServiceFn fn, std::uint32_t flags = 0) {
        services.add(KernelServiceInfo{std::move(name), std::move(fn), flags, {}, "kernel"});
    };
    const auto noop = [&services](std::string name, std::int32_t result, std::string note,
                                  std::uint32_t flags = 0) {
        services.add(KernelServiceInfo{std::move(name), [result](KernelCallContext& c) { c.ret(result); },
                                       flags | kernel_service_noop, std::move(note), "kernel"});
    };

    // SetupThread(gp, stack, stack_size, args, root): the stack argument is the stack's lowest address (-1:
    // the top of RAM minus the size); the result is the main thread's initial $sp. The argument block is left
    // as the program's crt0 cleared it (argc 0).
    add("SetupThread", [&kernel](KernelCallContext& c) {
        const std::uint32_t gp = c.arg(0);
        const std::uint32_t stack = c.arg(1);
        const std::int32_t size = c.sarg(2);
        R5900Context& ctx = c.ctx();
        if (gp != 0) {
            set_gpr_lo(ctx, 28,
                       static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(gp))));
        }
        std::uint32_t sp = gpr_u32(ctx, 29);
        std::uint32_t base = 0;
        const std::uint32_t stack_size = size > 0 ? static_cast<std::uint32_t>(size) : 0;
        if (stack == 0xFFFFFFFFu) {
            sp = stack_size != 0 && stack_size < PS2_RAM_SIZE ? PS2_RAM_SIZE - stack_size : PS2_RAM_SIZE;
        } else if (stack != 0) {
            sp = stack + stack_size;
        }
        sp &= ~0xFu;
        if (stack == 0xFFFFFFFFu) {
            base = sp;
        } else if (stack != 0) {
            base = stack;
        }
        kernel.setup_current(base, stack_size, gpr_u32(ctx, 28));
        c.ret_u32(sp);
    });
    // SetupHeap(start, size): -1 (or 0) lets the heap run up to the main thread's stack.
    add("SetupHeap", [&kernel](KernelCallContext& c) {
        const std::uint32_t base = (c.arg(0) + 0xFu) & ~0xFu;
        const std::uint32_t size = c.arg(1);
        const std::uint32_t limit = kernel.main_stack_base() != 0 ? kernel.main_stack_base() : PS2_RAM_SIZE;
        std::uint32_t end = limit;
        if (size != 0 && size != 0xFFFFFFFFu) {
            end = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t{base} + size, limit));
        }
        if (end <= base) {
            end = limit;
        }
        kernel.system_state().heap_start = base;
        kernel.system_state().heap_end = end;
        c.ret_u32(base);
    });
    add("EndOfHeap", [&kernel](KernelCallContext& c) {
        const std::uint32_t end = kernel.system_state().heap_end;
        c.ret_u32(end != 0 ? end : (kernel.main_stack_base() != 0 ? kernel.main_stack_base() : PS2_RAM_SIZE));
    });
    add("GetMemorySize", [](KernelCallContext& c) { c.ret_u32(PS2_RAM_SIZE); });

    // ---- OSD configuration.
    add("GetOsdConfigParam", [&kernel](KernelCallContext& c) {
        if (!guest_block_ok(c.arg(0), 4)) {
            c.ret(kabi::error);
            return;
        }
        write_u32(c.mem(), c.arg(0), kernel.system_state().osd_config);
        c.ret(kabi::ok);
    });
    add("SetOsdConfigParam", [&kernel](KernelCallContext& c) {
        if (!guest_block_ok(c.arg(0), 4)) {
            c.ret(kabi::error);
            return;
        }
        KernelSystemState& s = kernel.system_state();
        s.osd_config = sanitize_osd_config(read_u32(c.mem(), c.arg(0)));
        s.osd_config2 = readable_config2(s);
        c.ret(kabi::ok);
    });
    add("GetOsdConfigParam2", [&kernel](KernelCallContext& c) {
        const std::uint32_t address = c.arg(0);
        const std::uint32_t bytes = std::min<std::uint32_t>(c.arg(1), 4);
        if (bytes != 0 && !guest_block_ok(address, bytes, 1)) {
            c.ret(kabi::error);
            return;
        }
        const std::uint32_t raw = readable_config2(kernel.system_state());
        for (std::uint32_t i = 0; i < bytes; ++i) {
            c.mem().store8(address + i, static_cast<std::uint8_t>(raw >> (8 * i)));
        }
        c.ret(kabi::ok);
    });
    add("SetOsdConfigParam2", [&kernel](KernelCallContext& c) {
        const std::uint32_t address = c.arg(0);
        const std::uint32_t bytes = std::min<std::uint32_t>(c.arg(1), 4);
        if (bytes == 0) {
            c.ret(kabi::ok);
            return;
        }
        if (!guest_block_ok(address, bytes, 1)) {
            c.ret(kabi::error);
            return;
        }
        KernelSystemState& s = kernel.system_state();
        std::uint32_t raw = readable_config2(s);
        for (std::uint32_t i = 0; i < bytes; ++i) {
            raw &= ~(0xFFu << (8 * i));
            raw |= std::uint32_t{c.mem().load8(address + i)} << (8 * i);
        }
        raw = sanitize_osd_config2(raw);
        s.osd_config2 = raw;
        std::uint32_t version = (s.osd_config >> 13) & 0x7u;
        std::uint32_t language = (s.osd_config >> 16) & 0x1Fu;
        if (bytes >= 3) {
            version = (raw >> 16) & 0xFFu;
        }
        if (bytes >= 4) {
            language = (raw >> 24) & 0xFFu;
        }
        std::uint32_t config = s.osd_config & ~((0x7u << 13) | (0x1Fu << 16));
        config |= (version & 0x7u) << 13;
        config |= (language & 0x1Fu) << 16;
        s.osd_config = sanitize_osd_config(config);
        c.ret(kabi::ok);
    });

    // ---- The kernel's syscall table and its patch helpers.
    // SetSyscall(number, handler): guest code at `handler` serves `number` from now on (0 removes it); the
    // handler word is also written into the guest's view of the table, where a negative number reaches below
    // it (how the SDK pokes kernel memory).
    add("SetSyscall", [&kernel](KernelCallContext& c) {
        const std::uint32_t number = c.arg(0);
        const std::uint32_t handler = c.arg(1);
        if (handler != 0) {
            kernel.check_kernel_ram_target(handler, 4, "SetSyscall's handler");
        }
        if (const auto entry = table_entry(number)) {
            write_u32(c.mem(), *entry, handler);
        }
        KernelServices& registry = c.machine().kernel();
        if (handler == 0) {
            registry.remove_guest_handler(static_cast<std::int32_t>(number));
        } else {
            registry.install_guest_handler(static_cast<std::int32_t>(number), handler);
        }
        c.ret(kabi::ok);
    });
    // GetEntryAddress is native_first: libkernel points it at kernel-mode patch code (SetSyscall(0x5B, ...))
    // and calls it to look up the patch's handlers. Natively it returns the handler the kernel already has,
    // which is what a kernel that needs no patch would hand back.
    add(
        "GetEntryAddress",
        [](KernelCallContext& c) {
            const auto entry = table_entry(c.arg(0));
            c.ret_u32(entry ? read_u32(c.mem(), *entry) : 0);
        },
        kernel_service_native_first);
    // Copy(dest, src, size): a kernel-mode memcpy. FindAddress and Copy are native_first: the SDK's start-up
    // code installs user-mode copies of both with SetSyscall before it uses them (M3 bring-up), and that
    // handler code is in no function map; the guest's syscall table still shows the program's handler.
    add(
        "Copy",
        [&kernel](KernelCallContext& c) {
            const std::uint32_t dest = c.arg(0);
            const std::uint32_t src = c.arg(1);
            const std::uint32_t size = c.arg(2);
            kernel.check_kernel_ram_target(dest, size, "Copy");
            if (size != 0) {
                if (!guest_block_ok(dest, size, 1) || !guest_block_ok(src, size, 1)) {
                    c.unimplemented("Copy outside guest RAM");
                }
                std::vector<std::uint8_t> bytes(size);
                c.mem().read_bytes(src, bytes);
                c.mem().write_bytes(dest, bytes);
            }
            c.ret(kabi::ok);
        },
        kernel_service_native_first);
    // FindAddress(start, end, target): the address of the first word in [start, end) equal to `target`
    // (kseg0/kseg1 spellings match), or 0.
    add(
        "FindAddress",
        [](KernelCallContext& c) {
            const std::uint32_t start = (c.arg(0) + 3u) & ~3u;
            const std::uint32_t end = c.arg(1) & ~3u;
            const std::uint32_t target = c.arg(2);
            std::uint32_t found = 0;
            for (std::uint32_t a = start; a < end; a += 4) {
                if (!guest_block_ok(a, 4)) {
                    break;
                }
                const std::uint32_t word = read_u32(c.mem(), a);
                if (word == target || without_kseg(word) == without_kseg(target)) {
                    found = a;
                    break;
                }
            }
            c.ret_u32(found);
        },
        kernel_service_native_first);

    // ---- Documented noops (strict mode stops at them).
    const std::string coherent = "host memory is coherent: there is no cache to write back or invalidate";
    noop("FlushCache", kabi::ok, coherent + " (overlay residency comes from the loader hook, ADR-0012)");
    noop("iFlushCache", kabi::ok, coherent, kernel_service_interrupt_ok);
    noop("EnableCache", kabi::ok, coherent);
    noop("DisableCache", kabi::ok, coherent);
    noop("InitTLB", kabi::ok,
         "no TLB is modelled: guest memory is mapped directly, and TLB instructions stop");
    noop("SetVTLBRefillHandler", 0,
         "no TLB is modelled, so a refill never happens; the result is the previous handler, none");
    noop("Deci2Call", kabi::error, "a retail console has no DECI2 debugger: every request fails with -1",
         kernel_service_interrupt_ok);
    noop("SetMemoryMode", kabi::ok,
         "the memory mode selects how much RAM the kernel maps (32 MiB for retail programs); guest memory is "
         "always the console's 32 MiB, so there is nothing to change");

    // ResetEE(units): the EE units the bit field names go back to their power-on state. Accepted, as a noop,
    // when every named unit is idle: after boot an idle unit is what a reset leaves as far as a program can
    // see. Resetting a unit in the middle of a transfer is not modelled and stops. Bits (as the EE kernel's
    // documentation gives them; to confirm in bring-up): 0 DMAC, 1 VIF0, 2 VIF1, 3 GIF, 4 VU0, 5 VU1, 6 IPU.
    // The VUs are always idle here: their programs run to the end when they start.
    add("ResetEE", [](KernelCallContext& c) {
        const std::uint32_t units = c.arg(0);
        if ((units & ~reset_ee_units) != 0) {
            std::string message = "ResetEE(" + hex32(units) + ") names units this kernel does not know";
            stop_unsupported_call(c, std::move(message));
        }
        for (const ResetUnit& unit : reset_units) {
            if ((units & unit.bit) == 0) {
                continue;
            }
            for (std::uint32_t channel = 0; channel < 10; ++channel) {
                if ((unit.channels & (1u << channel)) == 0) {
                    continue;
                }
                std::uint8_t bytes[4] = {};
                c.machine().mmio().read(dmac_channel_base(channel), 4, bytes,
                                        MmioContext{c.ctx().pc, &c.ctx()});
                std::uint32_t chcr = 0;
                std::memcpy(&chcr, bytes, sizeof chcr);
                if ((chcr & chcr_str) != 0) {
                    std::string message = "ResetEE(" + hex32(units) + ") while the " +
                                          std::string(unit.name) + " is busy (DMA channel " +
                                          std::to_string(channel) +
                                          " running): resetting a working unit is not supported";
                    stop_unsupported_call(c, std::move(message));
                }
            }
        }
        c.ret(kabi::ok);
    });

    // ---- The exit paths stop the run. (Messages are built in their own statements: temporaries of the
    // statement that stops would never be destroyed, since the fiber is abandoned.)
    add("LoadExecPS2", [](KernelCallContext& c) {
        std::string message = "LoadExecPS2(\"" + read_guest_string(c.mem(), c.arg(0), 256) + "\", argc " +
                              std::to_string(c.arg(1)) +
                              ") leaves the program: loading another executable is not supported";
        stop_exit_path(c, std::move(message));
    });
    add("ExecOSD",
        [](KernelCallContext& c) { stop_exit_path(c, "ExecOSD leaves the program for the system browser"); });
}

} // namespace dq8::rt
