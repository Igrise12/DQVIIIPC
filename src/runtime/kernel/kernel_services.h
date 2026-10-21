// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/abi/ps2recomp/r5900_context.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The EE kernel as native services (architecture §5.1; replaces M2's KernelHle). A syscall instruction
// arrives by number and a by-name call from generated code (ps2_syscalls::Name, ps2_stubs::Name) by name;
// both resolve here to one service. Resolution order for a call:
//   1. a guest handler the program installed for that number with SetSyscall (run as a nested guest call that
//      returns to the instruction after the syscall);
//   2. the native service registered under the canonical name;
//   3. nothing: the caller stops the guest with a diagnostic. An unimplemented call never returns 0 silently.
// Services are registered by the devices that own them (kernel threads and sync by the kernel, GS calls by
// the GS registers, SIF calls by the IOP link) from their attach().
namespace dq8::rt {

class MachineServices;
class GuestMemory;

// One call in progress: the arguments, the result, and the machine.
class KernelCallContext {
  public:
    KernelCallContext(MachineServices& machine, R5900Context& ctx, std::string_view name,
                      std::string_view called_as, bool has_number, std::int32_t number,
                      bool interrupt_context) noexcept
        : machine_(machine), ctx_(ctx), name_(name), called_as_(called_as), has_number_(has_number),
          number_(number), interrupt_context_(interrupt_context) {}

    MachineServices& machine() const noexcept { return machine_; }
    GuestMemory& mem() const;
    R5900Context& ctx() const noexcept { return ctx_; }
    // The canonical service name, and the name or alias the guest used.
    std::string_view name() const noexcept { return name_; }
    std::string_view called_as() const noexcept { return called_as_; }
    bool has_number() const noexcept { return has_number_; }
    std::int32_t number() const noexcept { return number_; } // negative for an interrupt-context number
    // True for an i-variant: a negative number, or a by-name call through an interrupt-context alias.
    bool interrupt_context() const noexcept { return interrupt_context_; }

    // Argument i, 0..7: $a0..$a3 then $t0..$t3 (the EE's eight argument registers).
    std::uint32_t arg(unsigned i) const;
    std::int32_t sarg(unsigned i) const { return static_cast<std::int32_t>(arg(i)); }
    std::uint64_t arg64(unsigned i) const;

    // The result in $v0. 32-bit results are sign-extended into the low 64 bits as the kernel's return
    // sequence leaves them; the upper 64 bits of the register are kept.
    void ret(std::int32_t value) noexcept;
    void ret_u32(std::uint32_t value) noexcept { ret(static_cast<std::int32_t>(value)); }
    void ret64(std::uint64_t value) noexcept;

    // Stops the guest: the call reached a case the service does not implement.
    [[noreturn]] void unimplemented(std::string_view why) const;

  private:
    MachineServices& machine_;
    R5900Context& ctx_;
    std::string_view name_;
    std::string_view called_as_;
    bool has_number_;
    std::int32_t number_;
    bool interrupt_context_;
};

enum KernelServiceFlag : std::uint32_t {
    kernel_service_noop = 1u << 0,     // does nothing on purpose; `note` says why (strict mode stops instead)
    kernel_service_blocking = 1u << 1, // may block the calling thread
    kernel_service_interrupt_ok = 1u << 2, // valid in interrupt context
};

using KernelServiceFn = std::function<void(KernelCallContext&)>;

struct KernelServiceInfo {
    std::string name;   // canonical: the kernel_calls.h name, or the call-list name for a stub
    KernelServiceFn fn; // may be empty only for a noop
    std::uint32_t flags = 0;
    std::string note;  // required for a noop: why doing nothing is correct
    std::string owner; // the device that registered it, for the coverage table
};

// Where a call goes.
struct KernelResolution {
    enum class Kind { native, guest_handler, missing };
    Kind kind = Kind::missing;
    const KernelServiceInfo* service = nullptr; // native
    std::uint32_t guest_address = 0;            // guest_handler
    std::string_view name;                      // canonical name, or the name as called when unknown
    bool has_number = false;
    std::int32_t number = 0;
    bool interrupt_context = false;
};

class KernelServices {
  public:
    // Installs the standard SDK spelling aliases (SetGsCrt, isceSifSetDma, isceSifSetDChain).
    KernelServices();

    // Throws std::invalid_argument for a duplicate or empty name, a missing function on a non-noop, or a noop
    // without a note.
    void add(KernelServiceInfo info);
    // A by-name spelling for a canonical name. `interrupt_variant` marks calls through it as interrupt
    // context.
    void alias(std::string alias_name, std::string canonical, bool interrupt_variant = false);

    // A syscall instruction's number as the guest passed it (negative numbers as two's complement).
    KernelResolution resolve_number(std::uint32_t number) const;
    // A by-name call. A name with a kernel call number also honours SetSyscall handlers for that number.
    KernelResolution resolve_name(std::string_view name) const;

    // SetSyscall: guest code at `address` now serves `number`.
    void install_guest_handler(std::int32_t number, std::uint32_t address);
    void remove_guest_handler(std::int32_t number);
    std::optional<std::uint32_t> guest_handler(std::int32_t number) const;

    // A registered service by canonical name, or null.
    const KernelServiceInfo* find(std::string_view name) const;
    // Every registered service, sorted by name.
    std::vector<const KernelServiceInfo*> services() const;

    // Strict mode turns every noop into a stop, to audit them.
    void set_strict(bool strict) noexcept { strict_ = strict; }
    bool strict() const noexcept { return strict_; }

    // Runs a native service and counts the call.
    void invoke(const KernelServiceInfo& service, KernelCallContext& call);
    std::uint64_t call_count(std::string_view name) const;

  private:
    struct Alias {
        std::string canonical;
        bool interrupt_variant = false;
    };
    KernelResolution resolve(std::string_view name, bool has_number, std::int32_t number,
                             bool interrupt) const;

    std::map<std::string, KernelServiceInfo, std::less<>> services_;
    std::map<std::string, Alias, std::less<>> aliases_;
    std::map<std::int32_t, std::uint32_t> guest_handlers_;
    std::map<std::string, std::uint64_t, std::less<>> counts_;
    bool strict_ = false;
};

} // namespace dq8::rt
