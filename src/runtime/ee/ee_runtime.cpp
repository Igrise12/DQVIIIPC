// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/ee_runtime.h"

#include "runtime/ee/context.h"
#include "runtime/kernel/kernel_calls.h"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>

namespace dq8::rt {

namespace {

constexpr unsigned reg_v1 = 3;
constexpr unsigned reg_a0 = 4;

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
std::int32_t api_ee_checkpoint_due(void* /*host*/, std::uint32_t /*cycles*/) {
    return 0; // no virtual clock in M2
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
void api_execute_vu0_microprogram(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx,
                                  std::uint32_t address) {
    self(host).stop_unsupported(ctx, "VU0 microprogram call", address);
}
void api_vu0_start_microprogram(void* host, std::uint8_t* /*rdram*/, R5900Context* ctx,
                                std::uint32_t address) {
    self(host).stop_unsupported(ctx, "VU0 microprogram start", address);
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

EeRuntime::EeRuntime(GuestMemory& memory, const Dispatcher& dispatcher, const KernelHle& kernel)
    : memory_(memory), dispatcher_(dispatcher), kernel_(kernel), ctx_(std::make_unique<R5900Context>()) {
    adapter_.api = &api_table;
    adapter_.host = this;
    trace_ = [](std::string_view line) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    };
}

void EeRuntime::count_dispatch(std::uint32_t target, std::uint32_t source, std::string_view how,
                               std::string_view unit) {
    if (options_.max_dispatches != 0 && dispatches_ >= options_.max_dispatches) {
        StopInfo s;
        s.reason = StopReason::max_dispatches;
        s.pc = ctx_->pc;
        s.address = target;
        s.source = source;
        s.message = "dispatch budget of " + std::to_string(options_.max_dispatches) + " used up before " +
                    std::string(how) + " to " + hex32(target);
        throw GuestStop(std::move(s));
    }
    ++dispatches_;
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

void EeRuntime::stop_missing(std::uint32_t target, std::uint32_t source, std::string_view how) {
    StopInfo s;
    s.reason = StopReason::missing_function;
    s.pc = ctx_->pc;
    s.address = target;
    s.source = source;
    s.message = dispatcher_.describe_miss(target);
    if (how == "run-loop") {
        s.message += " (reached from the run loop: a jump unwound to it, or a return or fall-through set pc)";
    } else {
        s.message += " (" + std::string(how) + " from " + hex32(source) + ")";
    }
    throw GuestStop(std::move(s));
}

bool EeRuntime::dispatch_guest_branch(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t target_pc,
                                      std::uint32_t source_pc, std::uint32_t fallthrough_pc,
                                      std::uint32_t kind, const char* debug_name) {
    ctx->pc = target_pc;
    const bool is_call = kind == DQ8_BRANCH_DIRECT_CALL || kind == DQ8_BRANCH_INDIRECT_CALL;
    const std::string_view how =
        debug_name != nullptr ? std::string_view(debug_name) : branch_kind_name(kind);
    // A return (strict-return builds only) and a jump to 0 unwind to the run loop without a table lookup: the
    // run loop resumes through the resume entry, or ends the program at pc 0.
    if (!is_call && (kind == DQ8_BRANCH_RETURN || target_pc == 0)) {
        count_dispatch(target_pc, source_pc, how, {});
        return false;
    }
    const DispatchTarget target = dispatcher_.resolve(target_pc);
    count_dispatch(target_pc, source_pc, how, target.unit);
    if (target.fn == nullptr) {
        stop_missing(target_pc, source_pc, how);
    }
    if (!is_call) {
        return false; // the generated caller returns; the run loop continues at ctx->pc
    }
    if (depth_ >= options_.max_call_depth) {
        StopInfo s;
        s.reason = StopReason::call_depth;
        s.pc = ctx->pc;
        s.address = target_pc;
        s.source = source_pc;
        s.message = "more than " + std::to_string(options_.max_call_depth) +
                    " nested guest calls; the host stack guard stopped the guest at " + std::string(how) +
                    " " + hex32(source_pc) + " -> " + hex32(target_pc);
        throw GuestStop(std::move(s));
    }
    ++depth_;
    target.fn(rdram, ctx, &adapter_);
    --depth_;
    if (ctx->pc == 0) {
        return false; // the program ends; unwind to the run loop
    }
    if (ctx->pc == target_pc) {
        ctx->pc = fallthrough_pc; // pc unchanged: treated as a plain return
    }
    return ctx->pc == fallthrough_pc;
}

void EeRuntime::handle_syscall(std::uint8_t* rdram, R5900Context* ctx, std::uint32_t encoded_id) {
    const std::uint32_t number = encoded_id != 0 ? encoded_id : gpr_u32(*ctx, reg_v1);
    if (!options_.stop_at_first_syscall && kernel_.handle_syscall(number, rdram, ctx)) {
        return;
    }
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
                format_args(s.args) +
                (options_.stop_at_first_syscall ? "" : "; the kernel HLE has no handler for it");
    throw GuestStop(std::move(s));
}

void EeRuntime::call_named(std::uint32_t kind, const char* name, std::uint8_t* rdram, R5900Context* ctx) {
    const std::string_view call = name != nullptr ? std::string_view(name) : std::string_view();
    const bool stop_here =
        kind == DQ8_CALL_TODO || (kind == DQ8_CALL_SYSCALL && options_.stop_at_first_syscall);
    if (!stop_here && !call.empty() && kernel_.handle_named(call, rdram, ctx)) {
        return;
    }
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
                ", returning to " + hex32(ctx->pc) + ": " + format_args(s.args) +
                (kind == DQ8_CALL_TODO ? "; the recompiler left it unbound"
                 : stop_here           ? ""
                                       : "; the kernel HLE has no handler for it");
    throw GuestStop(std::move(s));
}

void EeRuntime::signal_exception(R5900Context* ctx, std::uint32_t exception) {
    StopInfo s;
    s.reason = StopReason::guest_exception;
    s.pc = ctx->pc;
    s.address = ctx->pc;
    s.number = exception;
    s.message = "guest exception: " + std::string(exception_name(exception)) + " (code " +
                std::to_string(exception) + ") at " + hex32(ctx->pc);
    throw GuestStop(std::move(s));
}

void EeRuntime::stop_unsupported(R5900Context* ctx, std::string_view what, std::uint32_t address) {
    StopInfo s;
    s.reason = StopReason::unsupported;
    s.pc = ctx->pc;
    s.address = address;
    s.message = std::string(what) + " at " + hex32(ctx->pc) + " is not supported in this build";
    if (address != 0) {
        s.message += " (address " + hex32(address) + ")";
    }
    throw GuestStop(std::move(s));
}

RunResult EeRuntime::run(const RunOptions& options) {
    options_ = options;
    dispatches_ = 0;
    depth_ = 0;
    last_branch_.clear();
    last_source_ = 0;
    last_target_ = 0;
    const auto finish = [this](StopInfo info) {
        info.last_branch = last_branch_;
        info.last_source = last_source_;
        info.last_target = last_target_;
        return RunResult{std::move(info), dispatches_};
    };
    for (;;) {
        try {
            const std::uint32_t pc = ctx_->pc;
            if (pc == 0) {
                StopInfo s;
                s.reason = StopReason::program_end;
                s.message = "pc reached 0: the entry function returned";
                return finish(std::move(s));
            }
            const DispatchTarget target = dispatcher_.resolve(pc);
            count_dispatch(pc, 0, "run-loop", target.unit);
            if (target.fn == nullptr) {
                stop_missing(pc, 0, "run-loop");
            }
            depth_ = 0;
            target.fn(memory_.rdram(), ctx_.get(), &adapter_);
        } catch (const GuestStop& stop) {
            return finish(stop.info());
        } catch (const std::exception& e) {
            StopInfo s;
            s.reason = StopReason::guest_error;
            s.pc = ctx_->pc;
            s.address = ctx_->pc;
            s.message = std::string("generated code threw at ") + hex32(ctx_->pc) + ": " + e.what();
            return finish(std::move(s));
        } catch (...) {
            StopInfo s;
            s.reason = StopReason::guest_error;
            s.pc = ctx_->pc;
            s.address = ctx_->pc;
            s.message = "generated code threw a non-standard exception at " + hex32(ctx_->pc);
            return finish(std::move(s));
        }
    }
}

} // namespace dq8::rt
