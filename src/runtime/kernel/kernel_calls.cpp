// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/kernel/kernel_calls.h"

#include <array>
#include <cstdio>

namespace dq8::rt {

namespace {

constexpr std::array calls{
    KernelCall{0x01, "ResetEE"},
    KernelCall{0x02, "GsSetCrt"},
    KernelCall{0x04, "ExitThread", true}, // SDK "Exit"; PS2Recomp ends the calling thread
    KernelCall{0x06, "LoadExecPS2"},
    KernelCall{0x0D, "SetVTLBRefillHandler"},
    KernelCall{0x10, "AddIntcHandler"},
    KernelCall{0x11, "RemoveIntcHandler"},
    KernelCall{0x12, "AddDmacHandler"},
    KernelCall{0x13, "RemoveDmacHandler"},
    KernelCall{0x14, "EnableIntc"},
    KernelCall{0x15, "DisableIntc"},
    KernelCall{0x16, "EnableDmac"},
    KernelCall{0x17, "DisableDmac"},
    KernelCall{0x18, "SetAlarm"},
    KernelCall{0x19, "CancelAlarm"},
    KernelCall{-0x1A, "iEnableIntc"},
    KernelCall{-0x1B, "iDisableIntc"},
    KernelCall{-0x1C, "iEnableDmac"},
    KernelCall{-0x1D, "iDisableDmac"},
    KernelCall{-0x1E, "iSetAlarm"},
    KernelCall{-0x1F, "iCancelAlarm"},
    KernelCall{0x20, "CreateThread"},
    KernelCall{0x21, "DeleteThread"},
    KernelCall{0x22, "StartThread"},
    KernelCall{0x23, "ExitThread"},
    KernelCall{0x24, "ExitDeleteThread"},
    KernelCall{0x25, "TerminateThread"},
    KernelCall{-0x26, "TerminateThread", true},
    KernelCall{0x29, "ChangeThreadPriority"},
    KernelCall{-0x2A, "iChangeThreadPriority"},
    KernelCall{0x2B, "RotateThreadReadyQueue"},
    KernelCall{-0x2C, "iRotateThreadReadyQueue"},
    KernelCall{0x2D, "ReleaseWaitThread"},
    KernelCall{-0x2E, "iReleaseWaitThread"},
    KernelCall{0x2F, "GetThreadId"},
    KernelCall{-0x2F, "GetThreadId", true},
    KernelCall{0x30, "ReferThreadStatus"},
    KernelCall{-0x31, "iReferThreadStatus"},
    KernelCall{0x32, "SleepThread"},
    KernelCall{0x33, "WakeupThread"},
    KernelCall{-0x34, "iWakeupThread"},
    KernelCall{0x35, "CancelWakeupThread"},
    KernelCall{-0x36, "iCancelWakeupThread"},
    KernelCall{0x37, "SuspendThread"},
    KernelCall{-0x38, "SuspendThread", true},
    KernelCall{0x39, "ResumeThread"},
    KernelCall{-0x3A, "ResumeThread", true},
    KernelCall{0x3C, "SetupThread"},
    KernelCall{0x3D, "SetupHeap"},
    KernelCall{0x3E, "EndOfHeap"},
    KernelCall{0x40, "CreateSema"},
    KernelCall{0x41, "DeleteSema"},
    KernelCall{0x42, "SignalSema"},
    KernelCall{-0x43, "iSignalSema"},
    KernelCall{0x44, "WaitSema"},
    KernelCall{0x45, "PollSema"},
    KernelCall{-0x46, "iPollSema"},
    KernelCall{0x47, "ReferSemaStatus"},
    KernelCall{-0x48, "iReferSemaStatus"},
    KernelCall{-0x49, "iDeleteSema"},
    KernelCall{0x4A, "SetOsdConfigParam"},
    KernelCall{0x4B, "GetOsdConfigParam"},
    KernelCall{0x50, "CreateEventFlag"},
    KernelCall{0x51, "DeleteEventFlag"},
    KernelCall{0x52, "SetEventFlag"},
    KernelCall{-0x53, "iSetEventFlag"},
    KernelCall{0x54, "ClearEventFlag"},
    KernelCall{-0x55, "iClearEventFlag"},
    KernelCall{0x56, "WaitEventFlag"},
    KernelCall{0x57, "PollEventFlag"},
    KernelCall{-0x58, "iPollEventFlag"},
    KernelCall{0x59, "ReferEventFlagStatus"},
    KernelCall{-0x5A, "iReferEventFlagStatus"},
    KernelCall{0x5A, "Copy"},
    KernelCall{0x5B, "GetEntryAddress"},
    KernelCall{0x5C, "EnableIntcHandler"},
    KernelCall{-0x5C, "EnableIntcHandler", true},
    KernelCall{0x5D, "DisableIntcHandler"},
    KernelCall{-0x5D, "DisableIntcHandler", true},
    KernelCall{0x5E, "EnableDmacHandler"},
    KernelCall{-0x5E, "EnableDmacHandler", true},
    KernelCall{0x5F, "DisableDmacHandler"},
    KernelCall{-0x5F, "DisableDmacHandler", true},
    KernelCall{0x61, "EnableCache"},
    KernelCall{0x62, "DisableCache"},
    KernelCall{0x64, "FlushCache"},
    KernelCall{-0x68, "iFlushCache"},
    KernelCall{0x6B, "sceSifStopDma"},
    KernelCall{0x6E, "SetOsdConfigParam2"},
    KernelCall{0x6F, "GetOsdConfigParam2"},
    KernelCall{0x70, "GsGetIMR"},
    KernelCall{-0x70, "iGsGetIMR"},
    KernelCall{0x71, "GsPutIMR"},
    KernelCall{-0x71, "iGsPutIMR"},
    KernelCall{0x73, "SetVSyncFlag"},
    KernelCall{0x74, "SetSyscall"},
    KernelCall{0x76, "sceSifDmaStat"},
    KernelCall{-0x76, "sceSifDmaStat", true},
    KernelCall{0x77, "sceSifSetDma"},
    KernelCall{-0x77, "sceSifSetDma", true},
    KernelCall{0x78, "sceSifSetDChain"},
    KernelCall{-0x78, "sceSifSetDChain", true},
    KernelCall{0x79, "sceSifSetReg"},
    KernelCall{-0x79, "sceSifSetReg", true},
    KernelCall{0x7A, "sceSifGetReg"},
    KernelCall{-0x7A, "sceSifGetReg", true},
    KernelCall{0x7B, "ExecOSD"},
    KernelCall{0x7C, "Deci2Call"},
    KernelCall{-0x7C, "Deci2Call", true},
    KernelCall{0x7F, "GetMemorySize"},
    KernelCall{0x80, "GetGsDxDyOffset"}, // _GetGsDxDyOffset in the SDK headers (to be confirmed)
    KernelCall{0x82, "InitTLB"},
    KernelCall{0x83, "FindAddress"},
    KernelCall{0x85, "SetMemoryMode"},
    KernelCall{0xFC, "SetAlarm", true},
    KernelCall{-0xFD, "iSetAlarm", true},
    KernelCall{0xFE, "CancelAlarm", true},
    KernelCall{-0xFF, "iCancelAlarm", true},
};

} // namespace

std::span<const KernelCall> kernel_calls() noexcept {
    return calls;
}

std::optional<std::string_view> kernel_call_name(std::uint32_t number) noexcept {
    const auto n = static_cast<std::int32_t>(number);
    for (const KernelCall& c : calls) {
        if (c.number == n) {
            return c.name;
        }
    }
    return std::nullopt;
}

std::optional<std::int32_t> kernel_call_number(std::string_view name) noexcept {
    for (const KernelCall& c : calls) {
        if (!c.alias && c.name == name) {
            return c.number;
        }
    }
    return std::nullopt;
}

std::string format_kernel_call_number(std::uint32_t number) {
    const auto n = static_cast<std::int32_t>(number);
    char buf[16];
    if (n < 0) {
        std::snprintf(buf, sizeof buf, "-0x%X", static_cast<unsigned>(-static_cast<std::int64_t>(n)));
    } else {
        std::snprintf(buf, sizeof buf, "0x%X", static_cast<unsigned>(n));
    }
    return buf;
}

} // namespace dq8::rt
