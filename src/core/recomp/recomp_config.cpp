// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/recomp/recomp_config.h"

#include "dq8/core/error.h"
#include "recomp/common.h"

#include <algorithm>
#include <toml.hpp>

// The emitter's call list. Session A's copy under include/dq8/abi/ps2recomp/ is the real one; until it is
// merged, src/core/CMakeLists.txt puts the stand-in under tests/unit/support/ on the include path.
#if __has_include("dq8/abi/ps2recomp/ps2_call_list.h")
#include "dq8/abi/ps2recomp/ps2_call_list.h"
#else
#include "ps2_call_list.h"
#endif

namespace dq8::core::recomp {

using detail::hex8;
using detail::path_utf8;

namespace {

#define DQ8_CALL_NAME(name) std::string_view{#name},
constexpr std::string_view syscall_names[] = {PS2_SYSCALL_LIST(DQ8_CALL_NAME)};
constexpr std::string_view stub_names[] = {PS2_STUB_LIST(DQ8_CALL_NAME)};
#undef DQ8_CALL_NAME

template <std::size_t N>
std::string_view find_alias(std::string_view name, const std::string_view (&list)[N]) {
    if (name.empty()) {
        return {};
    }
    for (const std::string_view entry : list) {
        if (entry == name) {
            return entry;
        }
    }
    if (name.front() == '_') {
        const std::string_view bare = name.substr(1);
        for (const std::string_view entry : list) {
            if (entry == bare) {
                return entry;
            }
        }
        return {};
    }
    for (const std::string_view entry : list) {
        if (entry.size() == name.size() + 1 && entry.front() == '_' && entry.substr(1) == name) {
            return entry;
        }
    }
    return {};
}

toml::ordered_value hex_value(std::uint32_t v) {
    toml::ordered_value value(static_cast<std::int64_t>(v));
    value.as_integer_fmt().fmt = toml::integer_format::hex;
    value.as_integer_fmt().uppercase = true;
    value.as_integer_fmt().width = 8;
    return value;
}

toml::ordered_value string_array(const std::vector<std::string>& items) {
    toml::ordered_value array(toml::ordered_array{});
    for (const std::string& s : items) {
        array.as_array().emplace_back(s);
    }
    array.as_array_fmt().fmt = items.size() > 4 ? toml::array_format::multiline : toml::array_format::oneline;
    return array;
}

} // namespace

RuntimeCall resolve_runtime_call(std::string_view map_name) {
    if (const std::string_view s = find_alias(map_name, syscall_names); !s.empty()) {
        return {RuntimeCallKind::syscall, s};
    }
    if (const std::string_view s = find_alias(map_name, stub_names); !s.empty()) {
        return {RuntimeCallKind::stub, s};
    }
    return {};
}

std::vector<std::string> stub_bindings(const FunctionMap& map) {
    std::vector<const MapFunction*> functions;
    for (const MapFunction& f : map.functions) {
        functions.push_back(&f);
    }
    std::stable_sort(functions.begin(), functions.end(),
                     [](const MapFunction* a, const MapFunction* b) { return a->start < b->start; });
    std::vector<std::string> out;
    for (const MapFunction* f : functions) {
        const RuntimeCall call = resolve_runtime_call(f->name);
        if (call.kind != RuntimeCallKind::none) {
            out.push_back(std::string(call.name) + "@" + hex8(f->start));
        }
    }
    return out;
}

RecompConfig make_recomp_config(const RecompConfigInput& in) {
    if (in.map == nullptr) {
        throw Error(ErrorCode::map_invalid, "make_recomp_config: unit '" + in.unit + "' has no function map");
    }
    RecompConfig config;
    config.stubs = stub_bindings(*in.map);
    for (const std::uint32_t a : in.map->link_return_entries) {
        config.entry_points.push_back(hex8(a));
    }
    config.jump_tables = in.map->jump_tables.size();

    toml::ordered_value general(toml::ordered_table{});
    general["input"] = path_utf8(in.elf);
    general["output"] = path_utf8(in.output_dir);
    general["ghidra_output"] = path_utf8(in.map_csv);
    general["single_file_output"] = in.single_file_output;
    general["patch_syscalls"] = in.patch_syscalls;
    general["patch_cop0"] = in.patch_cop0;
    general["patch_cache"] = in.patch_cache;
    general["stubs"] = string_array(config.stubs);
    general["entry_points"] = string_array(config.entry_points);
    general["skip"] = string_array(in.skip);

    toml::ordered_value root(toml::ordered_table{});
    root["general"] = std::move(general);
    if (!in.map->jump_tables.empty()) {
        toml::ordered_value tables(toml::ordered_array{});
        for (const JumpTable& t : in.map->jump_tables) {
            toml::ordered_value entries(toml::ordered_array{});
            for (const JumpTableEntry& e : t.entries) {
                toml::ordered_value entry(toml::ordered_table{});
                entry["index"] = static_cast<std::int64_t>(e.index);
                entry["target"] = hex_value(e.target);
                entry.as_table_fmt().fmt = toml::table_format::oneline;
                entries.as_array().push_back(std::move(entry));
            }
            entries.as_array_fmt().fmt = toml::array_format::multiline;
            toml::ordered_value table(toml::ordered_table{});
            table["address"] = hex_value(t.address);
            table["base_register"] = static_cast<std::int64_t>(t.base_register);
            table["entries"] = std::move(entries);
            tables.as_array().push_back(std::move(table));
        }
        tables.as_array_fmt().fmt = toml::array_format::array_of_tables;
        toml::ordered_value jump_tables(toml::ordered_table{});
        jump_tables["table"] = std::move(tables);
        jump_tables.as_table_fmt().fmt = toml::table_format::implicit;
        root["jump_tables"] = std::move(jump_tables);
    }

    config.toml =
        "# Recompiler configuration for code unit '" + in.unit + "' of region profile " + in.profile_id +
        ", written by\n# `dq8ctl recomp prepare` from the unit's function map. Regenerated on every run: "
        "edit the map, not\n# this file. It holds addresses from your disc: keep it local.\n" +
        toml::format(root);
    return config;
}

} // namespace dq8::core::recomp
