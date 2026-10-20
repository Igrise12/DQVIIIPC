/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 DQ8-Native contributors */
/*
 * The game-module ABI (architecture.md §2.1, §13.3; Docs/milestones/M2-first-recompile.md).
 *
 * A game module is one shared library per code unit (the main program, or one overlay), built on the user's
 * machine from recompiled code. It exports exactly one symbol, dq8_game_module_describe, which returns a
 * descriptor. The host hands generated code a PS2Runtime adapter (ps2recomp/ps2_runtime.h) that points at a
 * dq8_runtime_api table, so a module has no undefined references to the host.
 *
 * Plain C so that any compiler can build a module against it. Bump DQ8_GAME_MODULE_ABI_VERSION on any change
 * to these structures, to R5900Context, or to the meaning of a field, and record it in the ADR log.
 */
#ifndef DQ8_ABI_GAME_MODULE_H
#define DQ8_ABI_GAME_MODULE_H

#include <stdint.h>

#define DQ8_GAME_MODULE_MAGIC 0x4D384744u /* 'DG8M' read as a little-endian u32 */
#define DQ8_GAME_MODULE_ABI_VERSION 1u
#define DQ8_GAME_MODULE_DESCRIBE_SYMBOL "dq8_game_module_describe"

#if defined(_WIN32)
#define DQ8_MODULE_EXPORT __declspec(dllexport)
#else
#define DQ8_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
struct R5900Context;
class PS2Runtime;
/* The signature of every recompiled guest function (PS2Runtime::RecompiledFunction). */
typedef void (*dq8_guest_fn)(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern "C" {
#else
struct R5900Context;
typedef void (*dq8_guest_fn)(uint8_t* rdram, struct R5900Context* ctx, void* runtime);
#endif

enum dq8_unit_kind { DQ8_UNIT_MAIN = 1, DQ8_UNIT_OVERLAY = 2 };

/* Which namespace a by-name call came from (ps2_syscalls:: or ps2_stubs::). */
enum dq8_call_kind { DQ8_CALL_SYSCALL = 1, DQ8_CALL_STUB = 2, DQ8_CALL_TODO = 3 };

/* PS2Runtime::GuestBranchKind, as passed through dispatch_guest_branch. */
enum dq8_branch_kind {
    DQ8_BRANCH_DIRECT_JUMP = 0,
    DQ8_BRANCH_DIRECT_CALL = 1,
    DQ8_BRANCH_INDIRECT_JUMP = 2,
    DQ8_BRANCH_INDIRECT_CALL = 3,
    DQ8_BRANCH_RETURN = 4
};

typedef struct dq8_game_module_desc {
    uint32_t magic;            /* DQ8_GAME_MODULE_MAGIC */
    uint32_t abi_version;      /* DQ8_GAME_MODULE_ABI_VERSION the module was built against */
    uint32_t struct_size;      /* sizeof(dq8_game_module_desc) as compiled into the module */
    uint32_t unit_kind;        /* enum dq8_unit_kind */
    uint32_t overlay_id;       /* 0 for the main unit; the MWo3 header's overlay_id otherwise */
    const char* unit_name;     /* "main", "title", "battle", ... */
    const char* profile_id;    /* region profile id, e.g. "SLUS_21207" */
    const char* source_sha1;   /* 40 hex digits: the boot ELF, or the overlay container file */
    const char* generator;     /* e.g. "ps2recomp eb8688b2" plus "+patches:<digest>" when a queue applies */
    uint32_t context_size;     /* sizeof(R5900Context) as compiled into the module */
    uint32_t context_align;    /* alignof(R5900Context) */
    uint32_t entry_point;      /* guest entry for the main unit; 0 for overlays */
    uint32_t table_base;       /* guest address of slot 0, 4-aligned */
    uint32_t table_end;        /* one past the last covered address, 4-aligned */
    uint32_t table_slot_count; /* (table_end - table_base) / 4 */
    const dq8_guest_fn* table; /* table_slot_count entries; null slots are addresses with no entry */
} dq8_game_module_desc;

typedef const dq8_game_module_desc* (*dq8_game_module_describe_fn)(void);

/*
 * Host services, one function per PS2Runtime member that generated code calls. `host` is the opaque pointer
 * stored next to the table in PS2Runtime. Functions may unwind with a C++ exception in M2 (documented
 * deviation; see the milestone doc), so modules and host must share the C++ runtime library.
 */
typedef struct dq8_runtime_api {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*dispatch_guest_branch)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t target_pc,
                                     uint32_t source_pc, uint32_t fallthrough_pc, uint32_t kind,
                                     const char* debug_name);
    int32_t (*ee_checkpoint_due)(void* host, uint32_t cycles);
    void (*handle_syscall)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t encoded_id);
    void (*call_named)(void* host, uint32_t kind, const char* name, uint8_t* rdram, struct R5900Context* ctx);
    void (*signal_exception)(void* host, struct R5900Context* ctx, uint32_t exception);
    void (*handle_trap)(void* host, uint8_t* rdram, struct R5900Context* ctx);
    void (*handle_break)(void* host, uint8_t* rdram, struct R5900Context* ctx);
    void (*handle_tlbr)(void* host, uint8_t* rdram, struct R5900Context* ctx);
    void (*handle_tlbwi)(void* host, uint8_t* rdram, struct R5900Context* ctx);
    void (*handle_tlbwr)(void* host, uint8_t* rdram, struct R5900Context* ctx);
    void (*handle_tlbp)(void* host, uint8_t* rdram, struct R5900Context* ctx);
    void (*clear_llbit)(void* host, struct R5900Context* ctx);
    uint8_t (*load8)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr);
    uint16_t (*load16)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr);
    uint32_t (*load32)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr);
    uint64_t (*load64)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr);
    void (*load128)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr, uint64_t out[2]);
    void (*store8)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr, uint8_t value);
    void (*store16)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr, uint16_t value);
    void (*store32)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr, uint32_t value);
    void (*store64)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr, uint64_t value);
    void (*store128)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t vaddr, uint64_t lo,
                     uint64_t hi);
    void (*execute_vu0_microprogram)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t address);
    void (*vu0_start_microprogram)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t address);
    void (*kick_gif_dma_chain_from_mmio)(void* host, uint8_t* rdram, struct R5900Context* ctx, uint32_t d_pcr,
                                         uint32_t d_stat, uint32_t tadr, uint32_t chcr);
} dq8_runtime_api;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DQ8_ABI_GAME_MODULE_H */
