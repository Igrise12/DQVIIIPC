// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2025-2026 PS2Recomp contributors (ran-j/PS2Recomp)
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Syscalls/Interrupt.cpp (handler registration and the cause enables), re-signed
// to KernelCallContext. What changed: EnableIntc/DisableIntc and EnableDmac/DisableDmac edit the INTC mask
// and D_STAT's mask bits in the InterruptController instead of a software mask; SetVSyncFlag and the vsync
// waits are not here (the GS calls belong to session B, and the SDK's vsync waits are game code).
#include "runtime/ee/context.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/ee_services.h"
#include "runtime/kernel/kernel_abi.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

// The D_STAT mask bit of a DMAC cause (a channel, or the stall and MFIFO-empty causes), or 0.
std::uint32_t dmac_mask_bit(std::uint32_t cause) noexcept {
    if (cause < dma_channel_count) {
        return 1u << (16 + cause);
    }
    if (cause == 13) {
        return InterruptController::dstat_sim;
    }
    if (cause == 14) {
        return InterruptController::dstat_meim;
    }
    return 0;
}

} // namespace

void register_interrupt_services(KernelServices& services, EeKernel& kernel) {
    const auto add = [&services](std::string name, KernelServiceFn fn, std::uint32_t flags = 0) {
        services.add(KernelServiceInfo{std::move(name), std::move(fn), flags, {}, "kernel"});
    };
    constexpr std::uint32_t irq_ok = kernel_service_interrupt_ok;

    // AddIntcHandler(cause, handler, next, arg): `next` 0 puts the handler first, anything else last; the
    // handler runs with the caller's $gp, $a0 = cause and $a1 = arg.
    const auto add_handler = [&kernel](bool dmac) {
        return [&kernel, dmac](KernelCallContext& c) {
            c.ret(kernel.add_handler(dmac, c.arg(0), c.arg(1), c.sarg(2), c.arg(3), gpr_u32(c.ctx(), 28)));
        };
    };
    add("AddIntcHandler", add_handler(false));
    add("AddDmacHandler", add_handler(true));
    services.alias("AddIntcHandler2", "AddIntcHandler");
    services.alias("AddDmacHandler2", "AddDmacHandler");
    add("RemoveIntcHandler",
        [&kernel](KernelCallContext& c) { c.ret(kernel.remove_handler(false, c.arg(0), c.sarg(1))); });
    add("RemoveDmacHandler",
        [&kernel](KernelCallContext& c) { c.ret(kernel.remove_handler(true, c.arg(0), c.sarg(1))); });

    // The cause enables edit the hardware masks. They return KE_OK as upstream does; whether the EE kernel
    // reports the previous state is to be confirmed in bring-up.
    const auto intc_enable = [](bool enable) {
        return [enable](KernelCallContext& c) {
            const std::uint32_t cause = c.arg(0);
            if (cause < intc_line_count) {
                InterruptController& irq = c.machine().interrupts();
                const std::uint32_t bit = 1u << cause;
                if (((irq.intc_mask() & bit) != 0) != enable) {
                    irq.write_intc_mask(bit); // a 1 toggles the mask bit
                }
            }
            c.ret(kabi::ok);
        };
    };
    add("EnableIntc", intc_enable(true));
    add("iEnableIntc", intc_enable(true), irq_ok);
    add("DisableIntc", intc_enable(false));
    add("iDisableIntc", intc_enable(false), irq_ok);
    const auto dmac_enable = [](bool enable) {
        return [enable](KernelCallContext& c) {
            if (const std::uint32_t bit = dmac_mask_bit(c.arg(0)); bit != 0) {
                InterruptController& irq = c.machine().interrupts();
                if (((irq.dmac_stat() & bit) != 0) != enable) {
                    irq.write_dmac_stat(bit); // a 1 in the upper half toggles the mask bit
                }
            }
            c.ret(kabi::ok);
        };
    };
    add("EnableDmac", dmac_enable(true));
    add("iEnableDmac", dmac_enable(true), irq_ok);
    add("DisableDmac", dmac_enable(false));
    add("iDisableDmac", dmac_enable(false), irq_ok);

    // Enable/DisableIntcHandler(cause, id) and the DMAC forms.
    const auto handler_enable = [&kernel](bool dmac, bool enable) {
        return [&kernel, dmac, enable](KernelCallContext& c) {
            c.ret(kernel.set_handler_enabled(dmac, c.sarg(1), enable));
        };
    };
    add("EnableIntcHandler", handler_enable(false, true), irq_ok);
    add("DisableIntcHandler", handler_enable(false, false), irq_ok);
    add("EnableDmacHandler", handler_enable(true, true), irq_ok);
    add("DisableDmacHandler", handler_enable(true, false), irq_ok);
}

} // namespace dq8::rt
