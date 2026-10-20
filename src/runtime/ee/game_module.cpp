// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/game_module.h"

#include "dq8/abi/ps2recomp/r5900_context.h"
#include "runtime/ee/guest_stop.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace dq8::rt {

namespace {

std::string kind_name(std::uint32_t kind) {
    switch (kind) {
    case DQ8_UNIT_MAIN:
        return "main";
    case DQ8_UNIT_OVERLAY:
        return "overlay";
    default:
        return "unknown kind " + std::to_string(kind);
    }
}

bool is_hex40(std::string_view s) noexcept {
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::string quoted(std::string_view s) {
    return "\"" + std::string(s) + "\"";
}

// Checks one string field: present, non-empty unless `may_be_empty`, and equal to `expected` when that is
// non-empty.
void check_string(std::vector<std::string>& problems, std::string_view field, const char* value,
                  std::string_view expected, bool may_be_empty = false) {
    if (value == nullptr) {
        problems.push_back(std::string(field) + " is null");
        return;
    }
    const std::string_view v(value);
    if (v.empty() && !(may_be_empty && expected.empty())) {
        problems.push_back(std::string(field) + " is empty");
    } else if (!expected.empty() && v != expected) {
        problems.push_back(std::string(field) + " is " + quoted(v) + ", expected " + quoted(expected));
    }
}

} // namespace

std::vector<std::string> validate_descriptor(const dq8_game_module_desc* desc, const UnitSpec& spec) {
    std::vector<std::string> p;
    if (desc == nullptr) {
        p.emplace_back("dq8_game_module_describe returned null");
        return p;
    }
    if (desc->magic != DQ8_GAME_MODULE_MAGIC) {
        p.push_back("magic is " + hex32(desc->magic) + ", expected " + hex32(DQ8_GAME_MODULE_MAGIC) +
                    ": not a DQ8 game module");
        return p;
    }
    if (desc->abi_version != DQ8_GAME_MODULE_ABI_VERSION) {
        p.push_back("built against module ABI " + std::to_string(desc->abi_version) +
                    ", this host uses ABI " + std::to_string(DQ8_GAME_MODULE_ABI_VERSION) +
                    ": rebuild the module");
        return p;
    }
    if (desc->struct_size != sizeof(dq8_game_module_desc)) {
        p.push_back("descriptor size is " + std::to_string(desc->struct_size) + " bytes, expected " +
                    std::to_string(sizeof(dq8_game_module_desc)) + ": rebuild the module");
        return p;
    }

    if (desc->unit_kind != spec.kind) {
        p.push_back("unit kind is " + kind_name(desc->unit_kind) + ", expected " + kind_name(spec.kind));
    }
    if (desc->overlay_id != spec.overlay_id) {
        p.push_back("overlay id is " + std::to_string(desc->overlay_id) + ", expected " +
                    std::to_string(spec.overlay_id));
    }
    check_string(p, "unit name", desc->unit_name, spec.name);
    // A developer-mode unit (no profile in the spec) may carry no profile id.
    check_string(p, "profile id", desc->profile_id, spec.profile_id, true);
    if (desc->source_sha1 == nullptr) {
        p.emplace_back("source SHA-1 is null");
    } else if (!is_hex40(desc->source_sha1)) {
        p.push_back("source SHA-1 " + quoted(desc->source_sha1) + " is not 40 hex digits");
    } else if (!spec.source_sha1.empty() && !iequals(desc->source_sha1, spec.source_sha1)) {
        p.push_back("source SHA-1 is " + std::string(desc->source_sha1) + ", expected " + spec.source_sha1 +
                    ": the module was built from a different file");
    }
    check_string(p, "generator", desc->generator, {});
    if (desc->context_size != sizeof(R5900Context) || desc->context_align != alignof(R5900Context)) {
        p.push_back("R5900Context is " + std::to_string(desc->context_size) + " bytes aligned to " +
                    std::to_string(desc->context_align) + " in the module, " +
                    std::to_string(sizeof(R5900Context)) + " aligned to " +
                    std::to_string(alignof(R5900Context)) + " in the host");
    }
    if (desc->entry_point != spec.entry_point) {
        p.push_back("entry point is " + hex32(desc->entry_point) + ", expected " + hex32(spec.entry_point));
    }

    const std::string range = "[" + hex32(desc->table_base) + ", " + hex32(desc->table_end) + ")";
    bool table_ok = true;
    if ((desc->table_base & 3u) != 0 || (desc->table_end & 3u) != 0) {
        p.push_back("table range " + range + " is not 4-aligned");
        table_ok = false;
    }
    if (desc->table_end <= desc->table_base) {
        p.push_back("table range " + range + " is empty");
        table_ok = false;
    } else if (desc->table_slot_count != (desc->table_end - desc->table_base) / 4u) {
        p.push_back("table slot count is " + std::to_string(desc->table_slot_count) + ", range " + range +
                    " needs " + std::to_string((desc->table_end - desc->table_base) / 4u));
        table_ok = false;
    }
    if (desc->table == nullptr) {
        p.emplace_back("table pointer is null");
        table_ok = false;
    }
    if (spec.range_end != 0 && (desc->table_base < spec.range_begin || desc->table_end > spec.range_end)) {
        p.push_back("table range " + range + " is outside the unit's range [" + hex32(spec.range_begin) +
                    ", " + hex32(spec.range_end) + ")");
    }
    if (table_ok && spec.kind == DQ8_UNIT_MAIN && desc->unit_kind == DQ8_UNIT_MAIN) {
        const std::uint32_t entry = desc->entry_point;
        if (entry < desc->table_base || entry >= desc->table_end || (entry & 3u) != 0) {
            p.push_back("entry point " + hex32(entry) + " is outside the table " + range);
        } else if (desc->table[(entry - desc->table_base) / 4u] == nullptr) {
            p.push_back("entry point " + hex32(entry) + " has no function in the table");
        }
    }
    return p;
}

GameModule GameModule::open(const std::filesystem::path& file) {
    DynamicLibrary library = DynamicLibrary::open(file);
    void* sym = library.symbol(DQ8_GAME_MODULE_DESCRIBE_SYMBOL);
    if (sym == nullptr) {
        throw ModuleError(file.string() + " does not export " + DQ8_GAME_MODULE_DESCRIBE_SYMBOL +
                          ": not a DQ8 game module");
    }
    dq8_game_module_describe_fn describe = nullptr;
    static_assert(sizeof describe == sizeof sym);
    std::memcpy(&describe, &sym, sizeof describe);
    const dq8_game_module_desc* desc = describe();
    if (desc == nullptr) {
        throw ModuleError(file.string() + ": " + DQ8_GAME_MODULE_DESCRIBE_SYMBOL + " returned null");
    }
    return GameModule(std::move(library), desc, file);
}

std::filesystem::path module_path(const std::filesystem::path& dir, std::string_view unit_name) {
    return dir / shared_library_file_name(std::string(module_name_prefix) + std::string(unit_name));
}

std::string unit_name_from_module_file(const std::filesystem::path& file) {
    const std::string marker = "@";
    const std::string pattern = shared_library_file_name(std::string(module_name_prefix) + marker);
    const std::size_t at = pattern.find(marker);
    const std::string head = pattern.substr(0, at);
    const std::string tail = pattern.substr(at + marker.size());
    const std::string name = file.filename().string();
    if (name.size() <= head.size() + tail.size() || name.compare(0, head.size(), head) != 0 ||
        name.compare(name.size() - tail.size(), tail.size(), tail) != 0) {
        return {};
    }
    return name.substr(head.size(), name.size() - head.size() - tail.size());
}

std::vector<std::filesystem::path> find_module_files(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && !unit_name_from_module_file(entry.path()).empty()) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

ModuleSet load_modules(std::span<const UnitSpec> specs, const std::filesystem::path& dir) {
    ModuleSet set;
    for (const UnitSpec& spec : specs) {
        const std::filesystem::path file = module_path(dir, spec.name);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            set.problems.push_back(spec.name + ": module not found: " + file.string());
            continue;
        }
        try {
            GameModule module = GameModule::open(file);
            const std::vector<std::string> problems = validate_descriptor(&module.descriptor(), spec);
            for (const std::string& problem : problems) {
                set.problems.push_back(spec.name + ": " + problem + " (" + file.filename().string() + ")");
            }
            if (problems.empty()) {
                set.units.push_back({spec, std::move(module)});
            }
        } catch (const std::exception& e) {
            set.problems.push_back(spec.name + ": " + e.what());
        }
    }
    return set;
}

} // namespace dq8::rt
