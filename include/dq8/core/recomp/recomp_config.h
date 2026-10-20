// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/recomp/function_map.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The recompiler configuration of one code unit: the TOML file `ps2_recomp <config>` reads (PS2Recomp's
// ConfigManager: [general] input, output, ghidra_output, single_file_output, patch_syscalls, patch_cop0,
// patch_cache, stubs, entry_points, skip, and [[jump_tables.table]]). Written with toml11, regenerated on
// every `dq8ctl recomp prepare`; the map is the thing to edit, never the config.
namespace dq8::core::recomp {

enum class RuntimeCallKind { none, syscall, stub };

struct RuntimeCall {
    RuntimeCallKind kind = RuntimeCallKind::none;
    std::string_view name; // the spelling in the call list (PS2_SYSCALL_LIST / PS2_STUB_LIST)
};

// Resolves a map name against the compiled-in call list (the module ABI's ps2_call_list.h) the way the
// recompiler's emitter does: an exact match, else the name with one leading underscore removed or added.
// Kernel calls win over library stubs.
RuntimeCall resolve_runtime_call(std::string_view map_name);

// "Name@0xADDR" for every map function whose name resolves to a runtime call, in address order. Name is the
// call list's spelling, so the emitter binds the function start to ps2_syscalls::Name or ps2_stubs::Name.
// (Library stub names are stubbed by the recompiler even without a binding; kernel-call names are not.)
std::vector<std::string> stub_bindings(const FunctionMap& map);

struct RecompConfigInput {
    std::string unit;                 // for the header comment
    std::string profile_id;           // for the header comment
    std::filesystem::path elf;        // [general].input: the unit ELF
    std::filesystem::path output_dir; // [general].output: the generated C++ directory
    std::filesystem::path map_csv;    // [general].ghidra_output: the unit's function map
    const FunctionMap* map = nullptr; // stubs, entry points and jump tables come from here
    bool patch_syscalls = false;      // syscalls stay calls into the runtime
    bool patch_cop0 = true;
    bool patch_cache = true;
    bool single_file_output = false; // one file per function: parallel, incremental module builds
    std::vector<std::string> skip;   // [general].skip selectors ("Name" or "Name@0xADDR")
};

struct RecompConfig {
    std::string toml;                      // the file's text
    std::vector<std::string> stubs;        // [general].stubs
    std::vector<std::string> entry_points; // [general].entry_points: the map's link-return entries
    std::size_t jump_tables = 0;           // [[jump_tables.table]] count
};

RecompConfig make_recomp_config(const RecompConfigInput& input);

} // namespace dq8::core::recomp
