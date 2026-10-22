// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/ee_runtime.h"

#include "runtime/ee/context.h"
#include "runtime/ee/dispatch_table.h"
#include "runtime/ee/overlay_manager.h"
#include "runtime/kernel/ee_kernel.h"
#include "runtime/kernel/kernel_calls.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"
#include "runtime/vu/vu0_runner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

constexpr unsigned reg_v0 = 2;
constexpr unsigned reg_v1 = 3;
constexpr unsigned reg_a0 = 4;
constexpr unsigned reg_ra = 31;
constexpr std::uint32_t status_exl = 1u << 1;

// Stack a dispatched call must leave free on its fiber: room for the stop path and for host services.
constexpr std::size_t call_headroom = std::size_t{256} << 10;

std::array<std::uint32_t, 4> argument_registers(const R5900Context& ctx) {
    return {gpr_u32(ctx, reg_a0), gpr_u32(ctx, reg_a0 + 1), gpr_u32(ctx, reg_a0 + 2),
            gpr_u32(ctx, reg_a0 + 3)};
}

std::string format_args(const std::array<std::uint32_t, 4>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        out += (i == 0 ? "a0=" : (" a" + std::to_string(i) + "=")) + hex32(args[i]);
    }
    return out;
}

std::string_view exception_name(std::uint32_t code) noexcept {
    switch (code) {
    case EXCEPTION_TLB_REFILL:
        return "TLB refill";
    case EXCEPTION_ADDRESS_ERROR_LOAD:
        return "address error (load)";
    case EXCEPTION_ADDRESS_ERROR_STORE:
        return "address error (store)";
    case EXCEPTION_SYSCALL:
        return "syscall";
    case EXCEPTION_BREAKPOINT:
        return "breakpoint";
    case EXCEPTION_RESERVED_INSTRUCTION:
        return "reserved instruction";
    case EXCEPTION_INTEGER_OVERFLOW:
        return "integer overflow";
    case EXCEPTION_TRAP:
        return "trap";
    default:
        return "unknown exception";
    }
}

// ---- Locating code for stop messages (M4 session D).

// The table that holds `pc`: the main unit's, or the resident overlay's for the overlay window; null else.
const DispatchTable* table_for(const Dispatcher& d, std::uint32_t pc) {
    if (d.main_table().covers(pc)) {
        return &d.main_table();
    }
    if (d.overlays().in_window(pc)) {
        if (const OverlayUnit* res = d.overlays().resident()) {
            return res->table.covers(pc) ? &res->table : nullptr;
        }
    }
    return nullptr;
}

// The entry of the function whose code holds `pc`: the nearest filled slot at or below pc gives the
// function (a resume entry points at its owner), whose lowest slot is the entry. nullopt when no function
// lies below pc in its table.
std::optional<std::uint32_t> function_entry(const DispatchTable& table, std::uint32_t pc) {
    std::uint32_t a = pc & ~3u;
    while (table.covers(a) && table.lookup(a) == nullptr) {
        if (a < table.base() + 4) {
            return std::nullopt;
        }
        a -= 4;
    }
    const dq8_guest_fn fn = table.lookup(a);
    if (fn == nullptr) {
        return std::nullopt;
    }
    while (a >= table.base() + 4 && table.lookup(a - 4) == fn) {
        a -= 4;
    }
    return a;
}

// "function 0x0013CC70 of main", or "no known function (unit main)".
std::string describe_function(const Dispatcher& d, std::uint32_t pc) {
    const DispatchTable* table = table_for(d, pc);
    if (table == nullptr) {
        return "outside every code unit";
    }
    const std::string& unit_name = table == &d.main_table() ? d.main_name() : d.overlays().resident()->name;
    if (const auto entry = function_entry(*table, pc)) {
        return "function " + hex32(*entry) + " of " + unit_name;
    }
    return "no known function of " + unit_name;
}

constexpr std::uint32_t op_special = 0x00;
constexpr std::uint32_t op_regimm = 0x01;
constexpr std::uint32_t op_j = 0x02;
constexpr std::uint32_t op_beq = 0x04;
constexpr std::uint32_t funct_jr = 0x08;
constexpr std::uint32_t funct_break = 0x0D;

bool is_break(std::uint32_t w) {
    return (w >> 26) == op_special && (w & 0x3Fu) == funct_break;
}
bool is_trap(std::uint32_t w) {
    const std::uint32_t op = w >> 26;
    if (op == op_special) {
        const std::uint32_t f = w & 0x3Fu;
        return f >= 0x30u && f <= 0x36u && f != 0x35u; // tge, tgeu, tlt, tltu, teq, tne
    }
    if (op == op_regimm) {
        const std::uint32_t rt = (w >> 16) & 0x1Fu;
        return rt >= 0x08u && rt <= 0x0Eu && rt != 0x0Du; // tgei, tgeiu, tlti, tltiu, teqi, tnei
    }
    return false;
}
// jr (not jalr), j, and beq $0, $0 (b): control never falls past them.
bool ends_straight_line(std::uint32_t w) {
    const std::uint32_t op = w >> 26;
    return (op == op_special && (w & 0x3Fu) == funct_jr) || op == op_j ||
           (op == op_beq && ((w >> 16) & 0x3FFu) == 0);
}

std::string_view trap_name(std::uint32_t w) {
    static constexpr std::array<std::string_view, 7> special{"tge", "tgeu", "tlt", "tltu", "teq", "?", "tne"};
    static constexpr std::array<std::string_view, 7> regimm{"tgei", "tgeiu", "tlti", "tltiu",
                                                            "teqi", "?",     "tnei"};
    return (w >> 26) == op_special ? special[(w & 0x3Fu) - 0x30u] : regimm[((w >> 16) & 0x1Fu) - 0x08u];
}

// What the conventional codes mean: compilers emit break 7 (or a trap with code 7) after a division by zero
// check and break 6 for an overflow check.
std::string code_meaning(std::uint32_t code) {
    switch (code) {
    case 7:
        return " (the compiler's division-by-zero check)";
    case 6:
        return " (the compiler's overflow check)";
    default:
        return "";
    }
}

// Generated code does not set pc before break and trap instructions: pc holds the function's entry or the
// last branch target. The faulting instruction is the first one of its kind from there on. Decodes it and
// says where it is; flags one that lies past an unconditional jump with no label in between, which straight
// code cannot reach (data the recompiler decoded as code, or a target the function map does not know).
std::string describe_exception_site(GuestMemory& memory, const Dispatcher& d, std::uint32_t pc, bool trap) {
    constexpr std::uint32_t max_scan = 1024; // instructions
    const DispatchTable* table = table_for(d, pc);
    bool unreachable = false;
    for (std::uint32_t i = 0, a = pc & ~3u; i < max_scan && a < 0x02000000u; ++i, a += 4) {
        if (i != 0 && table != nullptr && table->covers(a) && table->is_function_entry(a)) {
            break; // the next function
        }
        if (i != 0 && table != nullptr && table->covers(a) && table->lookup(a) != nullptr) {
            unreachable = false; // a label: reachable again
        }
        const std::uint32_t w = memory.load32(a);
        if (trap ? is_trap(w) : is_break(w)) {
            std::string out;
            if (trap) {
                const std::uint32_t code = (w >> 26) == op_special ? (w >> 6) & 0x3FFu : 0;
                out = std::string(trap_name(w)) + " (code " + std::to_string(code) + code_meaning(code) + ")";
            } else {
                const std::uint32_t code = (w >> 6) & 0xFFFFFu;
                out = "break " + std::to_string(code) + code_meaning(code);
            }
            out += " at " + hex32(a) + " in " + describe_function(d, a);
            if (unreachable) {
                out += "; straight-line code from " + hex32(pc) +
                       " cannot reach it: probably data decoded as code, or a jump target the function map "
                       "does not have";
            }
            return out;
        }
        if (ends_straight_line(w)) {
            ++i;
            a += 4; // the delay slot
            unreachable = true;
        }
    }
    return std::string("no ") + (trap ? "trap" : "break") + " instruction follows " + hex32(pc) + " in " +
           describe_function(d, pc) + ": probably data decoded as code";
}

bool handler_active(const ExecState& state, std::int32_t number) {
    return std::find(state.active_syscall_handlers.begin(), state.active_syscall_handlers.end(), number) !=
           state.active_syscall_handlers.end();
}

// The dq8_runtime_api trampolines: `host` is the EeRuntime the adapter was built by.
EeRuntime& self(void* host) noexcept {
    return *static_cast<EeRuntime*>(host);
}

std::int32_t api_dispatch_guest_branch(void* host, std::uint8_t* rdram, R5900Context* ctx,
                                       std::uint32_t target, std::uint32_t source, std::uint32_t fallthrough,
                                       std::uint32_t kind, const char* debug_name) {
    return self(host).dispatch_guest_branch(rdram, ctx, target, source, fallthrough, kind, debug_name) ? 1
                                                                                                       : 0;
}
std::int32_t api_ee_checkpoint_due(void* host, std::uint32_t cycles) {
    self(host).checkpoint(cycles);
    return 0; // any switch already happened inside the call: generated code never unwinds for it
}
void api_handle_syscall(void* host, std::uint8_t* rdram, R5900Context* ctx, std::uint32_t encoded_id) {
    self(host).handle_syscall(rdram, ctx, encoded_id);
}
void api_call_named(void* host, std::uint32_t kind, const char* name, std::uint8_t* rdram,
                    R5900Context* ctx) {
    self(host).call_named(kind, name, rdram, ctx);
}
void api_signal_exception(void* host, R5900Context* ctx, std::uint32_t exception) {
    self(host).signal_exception(ctx, exception);
}
void api_handle_trap(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    self(host).signal_exception(ctx, EXCEPTION_TRAP);
}
void api_handle_break(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    self(host).signal_exception(ctx, EXCEPTION_BREAKPOINT);
}
void api_handle_tlbr(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    self(host).stop_unsupported(ctx, "TLBR");
}
void api_handle_tlbwi(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    self(host).stop_unsupported(ctx, "TLBWI");
}
void api_handle_tlbwr(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    self(host).stop_unsupported(ctx, "TLBWR");
}
void api_handle_tlbp(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    self(host).stop_unsupported(ctx, "TLBP");
}
void api_clear_llbit(void* /*host*/, R5900Context* ctx) {
    ctx->llbit = 0;
    ctx->lladdr = 0;
}
std::uint8_t api_load8(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr) {
    return self(host).memory().load8(vaddr, ctx->pc);
}
std::uint16_t api_load16(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr) {
    return self(host).memory().load16(vaddr, ctx->pc);
}
std::uint32_t api_load32(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr) {
    return self(host).memory().load32(vaddr, ctx->pc);
}
std::uint64_t api_load64(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr) {
    return self(host).memory().load64(vaddr, ctx->pc);
}
void api_load128(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr,
                 std::uint64_t out[2]) {
    self(host).memory().load128(vaddr, out, ctx->pc);
}
void api_store8(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr,
                std::uint8_t value) {
    self(host).memory().store8(vaddr, value, ctx->pc);
}
void api_store16(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr,
                 std::uint16_t value) {
    self(host).memory().store16(vaddr, value, ctx->pc);
}
void api_store32(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr,
                 std::uint32_t value) {
    self(host).memory().store32(vaddr, value, ctx->pc);
}
void api_store64(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr,
                 std::uint64_t value) {
    self(host).memory().store64(vaddr, value, ctx->pc);
}
void api_store128(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t vaddr,
                  std::uint64_t lo, std::uint64_t hi) {
    self(host).memory().store128(vaddr, lo, hi, ctx->pc);
}
// VCALLMS and VCALLMSR (vu0_runner.h): the program runs to its end on the thread's VU0 registers, and its
// cycles are charged to the EE clock (VU0 runs at the EE clock). Without a VU device they stop.
void run_vu0_program(void* host, R5900Context* ctx, std::uint32_t address, const char* what) {
    EeRuntime& rt = self(host);
    IVu0Runner* vu0 = rt.machine().vu0();
    if (vu0 == nullptr) {
        rt.stop_unsupported(ctx, what, address);
    }
    const std::uint64_t cycles = vu0->run_from_ee(*ctx, address & 0xFF8u);
    rt.machine().clock().charge(cycles);
}
void api_execute_vu0_microprogram(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx,
                                  std::uint32_t address) {
    run_vu0_program(host, ctx, address, "VU0 microprogram call");
}
void api_vu0_start_microprogram(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx,
                                std::uint32_t address) {
    run_vu0_program(host, ctx, address, "VU0 microprogram start");
}
void api_kick_gif_dma_chain_from_mmio(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx,
                                      std::uint32_t /*d_pcr*/, std::uint32_t /*d_stat*/, std::uint32_t tadr,
                                      std::uint32_t /*chcr*/) {
    self(host).stop_unsupported(ctx, "GIF DMA chain kick", tadr);
}

constexpr dq8_runtime_api api_table{
    .struct_size = sizeof(dq8_runtime_api),
    .abi_version = DQ8_GAME_MODULE_ABI_VERSION,
    .dispatch_guest_branch = &api_dispatch_guest_branch,
    .ee_checkpoint_due = &api_ee_checkpoint_due,
    .handle_syscall = &api_handle_syscall,
    .call_named = &api_call_named,
    .signal_exception = &api_signal_exception,
    .handle_trap = &api_handle_trap,
    .handle_break = &api_handle_break,
    .handle_tlbr = &api_handle_tlbr,
    .handle_tlbwi = &api_handle_tlbwi,
    .handle_tlbwr = &api_handle_tlbwr,
    .handle_tlbp = &api_handle_tlbp,
    .clear_llbit = &api_clear_llbit,
    .load8 = &api_load8,
    .load16 = &api_load16,
    .load32 = &api_load32,
    .load64 = &api_load64,
    .load128 = &api_load128,
    .store8 = &api_store8,
    .store16 = &api_store16,
    .store32 = &api_store32,
    .store64 = &api_store64,
    .store128 = &api_store128,
    .execute_vu0_microprogram = &api_execute_vu0_microprogram,
    .vu0_start_microprogram = &api_vu0_start_microprogram,
    .kick_gif_dma_chain_from_mmio = &api_kick_gif_dma_chain_from_mmio,
};

} // namespace

std::string_view branch_kind_name(std::uint32_t kind) noexcept {
    switch (kind) {
    case DQ8_BRANCH_DIRECT_JUMP:
        return "DirectJump";
    case DQ8_BRANCH_DIRECT_CALL:
        return "DirectCall";
    case DQ8_BRANCH_INDIRECT_JUMP:
        return "IndirectJump";
    case DQ8_BRANCH_INDIRECT_CALL:
        return "IndirectCall";
    case DQ8_BRANCH_RETURN:
        return "Return";
    default:
        return "Unknown";
    }
}

std::string_view call_kind_name(std::uint32_t kind) noexcept {
    switch (kind) {
    case DQ8_CALL_SYSCALL:
        return "syscall";
    case DQ8_CALL_STUB:
        return "stub";
    case DQ8_CALL_TODO:
        return "todo";
    default:
        return "unknown";
    }
}

const dq8_runtime_api& EeRuntime::api() noexcept {
    return api_table;
}

EeRuntime::EeRuntime(GuestMemory& memory, const Dispatcher& dispatcher, MachineServices& machine,
                     KernelServices& kernel_services)
    : memory_(memory), dispatcher_(dispatcher), machine_(machine), services_(kernel_services),
      offline_(std::make_unique<ExecState>()), power_on_(std::make_unique<R5900Context>()) {
    adapter_.api = &api_table;
    adapter_.host = this;
    trace_ = [](std::string_view line) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    };
}

EeRuntime::~EeRuntime() = default;

R5900Context& EeRuntime::context() noexcept {
    if (exec_ != nullptr && exec_->ctx != nullptr) {
        return *exec_->ctx;
    }
    if (kernel_ != nullptr) {
        if (const ExecState* last = kernel_->last_exec(); last != nullptr && last->ctx != nullptr) {
            return *last->ctx;
        }
        return kernel_->boot_context();
    }
    return *power_on_;
}

ExecState& EeRuntime::state_for(R5900Context* ctx) {
    if (exec_ != nullptr) {
        return *exec_;
    }
    offline_->ctx = ctx;
    return *offline_;
}

void EeRuntime::annotate_stop(StopInfo& info) const {
    if (info.last_branch.empty()) {
        info.last_branch = last_branch_;
        info.last_source = last_source_;
        info.last_target = last_target_;
    }
}

void EeRuntime::stop(StopInfo info) {
    machine_.stop(std::move(info));
    std::abort(); // MachineServices::stop never returns; GCC does not trust [[noreturn]] through a virtual
                  // call
}

void EeRuntime::count_dispatch(std::uint32_t target, std::uint32_t source, std::string_view how,
                               std::string_view unit) {
    if (options_.max_dispatches != 0 && dispatches_ >= options_.max_dispatches) {
        StopInfo s;
        s.reason = StopReason::max_dispatches;
        s.pc = context().pc;
        s.address = target;
        s.source = source;
        s.message = "dispatch budget of " + std::to_string(options_.max_dispatches) + " used up before " +
                    std::string(how) + " to " + hex32(target);
        stop(std::move(s));
    }
    ++dispatches_;
    machine_.clock().charge(machine_.cost().dispatch);
    last_branch_ = how;
    last_source_ = source;
    last_target_ = target;
    if (options_.trace_dispatch && trace_) {
        std::string line = "dispatch #" + std::to_string(dispatches_) + " " + std::string(how) + " ";
        if (how != "run-loop") {
            line += hex32(source) + " ";
        }
        line += "-> " + hex32(target) + " [" + (unit.empty() ? std::string("miss") : std::string(unit)) + "]";
        trace_(line);
    }
}

void EeRuntime::stop_missing(std::uint32_t pc, std::uint32_t target, std::uint32_t source,
                             std::string_view how) {
    StopInfo s;
    s.reason = StopReason::missing_function;
    s.pc = pc;
    s.address = target;
    s.source = source;
    s.message = dispatcher_.describe_miss(target);
    // A jump (a jump table's target, typically) that leaves its own function names where it lands: the
    // function map splits one function into two, or misses a label.
    if (how != "run-loop" && source != 0 && how.find("Call") == std::string_view::npos) {
        const DispatchTable* from = table_for(dispatcher_, source);
        const DispatchTable* to = table_for(dispatcher_, target);
        const std::optional<std::uint32_t> own =
            from != nullptr ? function_entry(*from, source) : std::nullopt;
        const std::optional<std::uint32_t> lands = to != nullptr ? function_entry(*to, target) : std::nullopt;
        if (own && lands && from == to && *own != *lands) {
            s.message += "; the jump at " + hex32(source) + " in function " + hex32(*own) +
                         " lands outside its function, in function " + hex32(*lands) +
                         " (a jump-table target the function map gives to another function?)";
        } else if (own && lands && from == to) {
            s.message += "; the target lies inside the jumping function " + hex32(*own) +
                         " but is no label the recompiler knew (a jump-table target the map lacks?)";
        }
    }
    if (how == "run-loop") {
        s.message += " (reached from the run loop: a jump unwound to it, or a return or fall-through set pc)";
    } else {
        s.message += " (" + std::string(how) + " from " + hex32(source) + ")";
    }
    stop(std::move(s));
}

// ---- Dispatch.

bool EeRuntime::dispatch_guest_branch(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t target_pc,
                                      std::uint32_t source_pc, std::uint32_t fallthrough_pc,
                                      std::uint32_t kind, const char* debug_name) {
    ExecState& st = state_for(ctx);
    ctx->pc = target_pc;
    const bool is_call = kind == DQ8_BRANCH_DIRECT_CALL || kind == DQ8_BRANCH_INDIRECT_CALL;
    const std::string_view how =
        debug_name != nullptr ? std::string_view(debug_name) : branch_kind_name(kind);
    // A return (strict-return builds only) and a jump to 0 unwind to the run loop without a table lookup: the
    // run loop resumes through the resume entry, or ends the thread at pc 0.
    if (!is_call && (kind == DQ8_BRANCH_RETURN || target_pc == 0)) {
        count_dispatch(target_pc, source_pc, how, {});
        st.unwinding = true;
        return false;
    }
    const DispatchTarget target = dispatcher_.resolve(target_pc);
    count_dispatch(target_pc, source_pc, how, target.unit);
    if (target.fn == nullptr) {
        stop_missing(ctx->pc, target_pc, source_pc, how);
    }
    if (!is_call) {
        st.unwinding = true; // the generated callers return; the run loop continues at ctx->pc
        return false;
    }
    if (st.depth >= options_.max_call_depth ||
        (st.fiber != nullptr && st.fiber->headroom() < call_headroom)) {
        StopInfo s;
        s.reason = StopReason::call_depth;
        s.pc = ctx->pc;
        s.address = target_pc;
        s.source = source_pc;
        s.message = (st.depth >= options_.max_call_depth
                         ? "more than " + std::to_string(options_.max_call_depth) + " nested guest calls"
                         : "the fiber's stack has less than " + std::to_string(call_headroom >> 10) +
                               " KiB left after " + std::to_string(st.depth) + " nested guest calls") +
                    "; the host stack guard stopped the guest at " + std::string(how) + " " +
                    hex32(source_pc) + " -> " + hex32(target_pc);
        stop(std::move(s));
    }
    if (kernel_ != nullptr) {
        kernel_->safe_point(st); // may run other threads; the call below then happens when we are resumed
    }
    // Stale-return check (M3-boots.md, contract decision 2), in every build since M4: a call made from
    // overlay code must return into the overlay that made it. Two compares and a load per dispatched call.
    const OverlayManager& overlays = dispatcher_.overlays();
    const bool from_window = overlays.in_window(source_pc);
    const std::uint64_t generation = from_window ? overlays.generation() : 0;
    ++st.depth;
    target.fn(rdram, ctx, &adapter_);
    --st.depth;
    if (st.unwinding) {
        return false; // a jump or a mismatched return below us: keep unwinding to the run loop
    }
    if (ctx->pc != fallthrough_pc) {
        st.unwinding = true; // returned somewhere else (pc 0, a changed $ra, its own entry): unwind
        return false;
    }
    if (from_window && overlays.generation() != generation) {
        StopInfo s;
        s.reason = StopReason::stale_return;
        s.pc = ctx->pc;
        s.address = fallthrough_pc;
        s.source = source_pc;
        s.message = "stale return: the call at " + hex32(source_pc) + " in the overlay window returned to " +
                    hex32(fallthrough_pc) + " after the resident overlay changed (generation " +
                    std::to_string(generation) + " -> " + std::to_string(overlays.generation()) + ")";
        stop(std::move(s));
    }
    return true;
}

void EeRuntime::checkpoint(std::uint32_t cycles) {
    machine_.clock().charge(Cycles{cycles} * machine_.cost().backedge_scale);
    if (exec_ != nullptr && kernel_ != nullptr) {
        kernel_->safe_point(*exec_);
    }
}

// ---- Run loops.

void EeRuntime::run_from_pc(ExecState& st, R5900Context& ctx, std::uint32_t base_depth) {
    const std::uint32_t pc = ctx.pc;
    const DispatchTarget target = dispatcher_.resolve(pc);
    count_dispatch(pc, 0, "run-loop", target.unit);
    if (target.fn == nullptr) {
        stop_missing(pc, pc, 0, "run-loop");
    }
    st.unwinding = false;
    st.depth = base_depth;
    // Exceptions are caught here, on the fiber that threw them; the stop happens after the handler is left
    // (fiber.h, invariant 1).
    std::optional<StopInfo> caught;
    try {
        target.fn(memory_.rdram(), &ctx, &adapter_);
    } catch (const GuestStop& stop) {
        caught = stop.info();
    } catch (const std::exception& e) {
        caught.emplace();
        caught->reason = StopReason::guest_error;
        caught->pc = ctx.pc;
        caught->address = ctx.pc;
        caught->message = std::string("generated code threw at ") + hex32(ctx.pc) + ": " + e.what();
    } catch (...) {
        caught.emplace();
        caught->reason = StopReason::guest_error;
        caught->pc = ctx.pc;
        caught->address = ctx.pc;
        caught->message = "generated code threw a non-standard exception at " + hex32(ctx.pc);
    }
    if (caught) {
        stop(std::move(*caught));
    }
}

void EeRuntime::run_thread(ExecState& st, bool boot_thread) {
    for (;;) {
        if (kernel_ != nullptr) {
            kernel_->safe_point(st); // the run loop is a safe point too
        }
        R5900Context& ctx = *st.ctx;
        if (ctx.pc == 0) {
            if (boot_thread || kernel_ == nullptr) {
                StopInfo s;
                s.reason = StopReason::program_end;
                s.message = "pc reached 0: the entry function returned";
                stop(std::move(s));
            }
            kernel_->exit_current(false); // a thread's entry function returned: ExitThread
        }
        run_from_pc(st, ctx, 0);
    }
}

void EeRuntime::call_guest(ExecState& st, R5900Context& ctx) {
    R5900Context* const saved_ctx = st.ctx;
    const bool saved_unwinding = st.unwinding;
    const std::uint32_t base_depth = st.depth;
    st.ctx = &ctx;
    while (ctx.pc != 0) {
        if (kernel_ != nullptr) {
            kernel_->safe_point(st);
        }
        run_from_pc(st, ctx, base_depth);
    }
    st.ctx = saved_ctx;
    st.unwinding = saved_unwinding;
    st.depth = base_depth;
}

// ---- Calls from native override code (IGuestCalls).

namespace {

// The argument registers in order: $a0..$a3, then $t0..$t3.
constexpr std::array<unsigned, 8> argument_regs{4, 5, 6, 7, 8, 9, 10, 11};
constexpr unsigned reg_sp = 29;

void load_arguments(R5900Context& ctx, std::span<const std::uint64_t> args) {
    for (std::size_t i = 0; i < args.size() && i < argument_regs.size(); ++i) {
        set_gpr_lo(ctx, argument_regs[i], args[i]);
    }
}

} // namespace

std::uint64_t EeRuntime::call_function(R5900Context& ctx, std::uint32_t entry,
                                       std::span<const std::uint64_t> args, std::uint32_t sp) {
    // The same shape as a SetSyscall handler: a nested call on a copy of the caller's registers that returns
    // to pc 0. The copy lives in the thread's state, not in a local, so that a thread terminated inside the
    // callee leaves nothing behind that must be destroyed on this fiber.
    ExecState& st = state_for(&ctx);
    machine_.clock().charge(machine_.cost().dispatch);
    st.nested_contexts.push_back(std::make_unique<R5900Context>(ctx));
    R5900Context& nested = *st.nested_contexts.back();
    load_arguments(nested, args);
    set_gpr_lo(nested, reg_sp, sp);
    set_gpr_lo(nested, reg_ra, 0);
    nested.pc = entry;
    call_guest(st, nested);
    const std::uint64_t v0 = gpr_lo(nested, reg_v0);
    st.nested_contexts.pop_back();
    return v0;
}

std::uint64_t EeRuntime::call_kernel(R5900Context& ctx, std::string_view name,
                                     std::span<const std::uint64_t> args) {
    const KernelServiceInfo* service = services_.find(name);
    if (service == nullptr) {
        stop_unsupported(&ctx, "a native kernel call to " + std::string(name) + ", which has no service");
    }
    ExecState& st = state_for(&ctx);
    machine_.clock().charge(machine_.cost().kernel_call);
    st.nested_contexts.push_back(std::make_unique<R5900Context>(ctx));
    R5900Context& nested = *st.nested_contexts.back();
    load_arguments(nested, args);
    KernelCallContext call(machine_, nested, service->name, name, false, 0, false);
    services_.invoke(*service, call);
    trace_kernel_call(service->name, nested);
    const std::uint64_t v0 = gpr_lo(nested, reg_v0);
    st.nested_contexts.pop_back();
    return v0;
}

// ---- Kernel calls.

void EeRuntime::run_syscall_handler(ExecState& st, R5900Context& ctx, std::int32_t number,
                                    std::uint32_t handler) {
    // The handler runs as a nested call on a copy of the caller's registers (the kernel's saved user context)
    // and returns to pc 0; only $v0 comes back. A call of the same number inside it reaches the native
    // service.
    st.nested_contexts.push_back(std::make_unique<R5900Context>(ctx));
    R5900Context& nested = *st.nested_contexts.back();
    nested.pc = handler;
    nested.cop0_status |= status_exl;
    set_gpr_lo(nested, reg_ra, 0);
    st.active_syscall_handlers.push_back(number);
    call_guest(st, nested);
    std::memcpy(&ctx.r[reg_v0], &nested.r[reg_v0], sizeof ctx.r[reg_v0]);
    st.active_syscall_handlers.pop_back();
    st.nested_contexts.pop_back();
}

void EeRuntime::trace_kernel_call(std::string_view name, const R5900Context& ctx) {
    if (machine_.tracing(TraceCategory::kernel)) {
        machine_.trace(TraceCategory::kernel, std::string(name) + "(" + format_args(argument_registers(ctx)) +
                                                  ") -> " + hex32(gpr_u32(ctx, reg_v0)));
    }
}

void EeRuntime::handle_syscall(std::uint8_t* /*rdram*/, R5900Context* ctx, std::uint32_t encoded_id) {
    ExecState& st = state_for(ctx);
    const std::uint32_t number = encoded_id != 0 ? encoded_id : gpr_u32(*ctx, reg_v1);
    const auto n = static_cast<std::int32_t>(number);
    machine_.clock().charge(machine_.cost().kernel_call);
    const auto stop_here = [&](std::string_view suffix) {
        const auto name = kernel_call_name(number);
        StopInfo s;
        s.reason = StopReason::syscall;
        s.pc = ctx->pc;
        s.address = ctx->pc - 4u; // generated code sets pc to the instruction after the syscall first
        s.has_number = true;
        s.number = number;
        s.call_name = name ? std::string(*name) : std::string();
        s.call_kind = DQ8_CALL_SYSCALL;
        s.args = argument_registers(*ctx);
        s.message = "syscall " + format_kernel_call_number(number) + " (" +
                    (name ? std::string(*name) : std::string("unknown")) + ") at " + hex32(s.address) + ": " +
                    format_args(s.args) + std::string(suffix);
        stop(std::move(s));
    };
    if (options_.stop_at_first_syscall) {
        stop_here("");
    }
    const KernelResolution r = services_.resolve_number(number);
    if (r.kind == KernelResolution::Kind::guest_handler && !handler_active(st, n)) {
        run_syscall_handler(st, *ctx, n, r.guest_address);
        if (kernel_ != nullptr) {
            kernel_->after_kernel_call(st);
        }
        return;
    }
    const KernelServiceInfo* service = r.service;
    if (r.kind == KernelResolution::Kind::guest_handler) {
        const auto name = kernel_call_name(number);
        service = name ? services_.find(*name) : nullptr;
    }
    if (service == nullptr) {
        stop_here("; the kernel HLE has no handler for it");
    }
    KernelCallContext call(machine_, *ctx, service->name,
                           r.name.empty() ? std::string_view(service->name) : r.name, true, n,
                           r.interrupt_context);
    if (kernel_ != nullptr && !kernel_->in_handler()) {
        kernel_->note_progress();
    }
    services_.invoke(*service, call);
    trace_kernel_call(service->name, *ctx);
    if (kernel_ != nullptr) {
        kernel_->after_kernel_call(st);
    }
}

void EeRuntime::call_named(std::uint32_t kind, const char* name, std::uint8_t* /*rdram*/, R5900Context* ctx) {
    ExecState& st = state_for(ctx);
    const std::string_view call = name != nullptr ? std::string_view(name) : std::string_view();
    const auto stop_here = [&](std::string_view suffix) {
        StopInfo s;
        s.reason = StopReason::named_call;
        s.pc = ctx->pc;
        s.call_name = std::string(call);
        s.call_kind = kind;
        s.args = argument_registers(*ctx);
        if (kind == DQ8_CALL_SYSCALL) {
            if (const auto number = kernel_call_number(call)) {
                s.has_number = true;
                s.number = static_cast<std::uint32_t>(*number);
            }
        }
        s.message = std::string(call_kind_name(kind)) + " call " +
                    (call.empty() ? std::string("<unnamed>") : s.call_name) +
                    (s.has_number ? " (" + format_kernel_call_number(s.number) + ")" : std::string()) +
                    ", returning to " + hex32(ctx->pc) + ": " + format_args(s.args) + std::string(suffix);
        stop(std::move(s));
    };
    if (kind == DQ8_CALL_TODO) {
        stop_here("; the recompiler left it unbound");
    }
    machine_.clock().charge(machine_.cost().kernel_call);
    if (kind == DQ8_CALL_SYSCALL && options_.stop_at_first_syscall) {
        stop_here("");
    }
    const KernelResolution r = call.empty() ? KernelResolution{} : services_.resolve_name(call);
    if (r.kind == KernelResolution::Kind::guest_handler && !handler_active(st, r.number)) {
        run_syscall_handler(st, *ctx, r.number, r.guest_address);
        if (kernel_ != nullptr) {
            kernel_->after_kernel_call(st);
        }
        return;
    }
    const KernelServiceInfo* service =
        r.kind == KernelResolution::Kind::guest_handler ? services_.find(r.name) : r.service;
    if (service == nullptr) {
        stop_here("; the kernel HLE has no handler for it");
    }
    // A by-name call has no syscall number of its own (KernelCallContext: has_number means a syscall
    // instruction); an i-form name still marks interrupt context.
    KernelCallContext c(machine_, *ctx, service->name, call, false, 0, r.interrupt_context);
    if (kernel_ != nullptr && !kernel_->in_handler()) {
        kernel_->note_progress();
    }
    services_.invoke(*service, c);
    trace_kernel_call(call, *ctx);
    if (kernel_ != nullptr) {
        kernel_->after_kernel_call(st);
    }
}

void EeRuntime::signal_exception(R5900Context* ctx, std::uint32_t exception) {
    StopInfo s;
    s.reason = StopReason::guest_exception;
    s.pc = ctx->pc;
    s.address = ctx->pc;
    s.number = exception;
    s.message = "guest exception: " + std::string(exception_name(exception)) + " (code " +
                std::to_string(exception) + ") at " + hex32(ctx->pc);
    if (exception == EXCEPTION_BREAKPOINT || exception == EXCEPTION_TRAP) {
        s.message +=
            ": " + describe_exception_site(memory_, dispatcher_, ctx->pc, exception == EXCEPTION_TRAP);
    } else {
        s.message += " in " + describe_function(dispatcher_, ctx->pc);
    }
    stop(std::move(s));
}

void EeRuntime::stop_unsupported(R5900Context* ctx, std::string_view what, std::uint32_t address) {
    StopInfo s;
    s.reason = StopReason::unsupported;
    s.pc = ctx->pc;
    s.address = address;
    s.message = std::string(what) + " at " + hex32(ctx->pc) + " (" + describe_function(dispatcher_, ctx->pc) +
                ") is not supported in this build";
    if (address != 0) {
        s.message += " (address " + hex32(address) + ")";
    }
    stop(std::move(s));
}

} // namespace dq8::rt
