// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The build registry (dq8/core/registry.h): installs.toml in the user data directory, one [[install]] per
// profile. Paths are stored as UTF-8 strings; keys a later version adds are ignored when reading.
#include "dq8/core/registry.h"

#include "dq8/core/error.h"
#include "platform/files.h"
#include "platform/paths.h"

#include <sstream>
#include <system_error>
#include <toml.hpp>
#include <utility>

namespace dq8::core::registry {

namespace fs = std::filesystem;

namespace {

constexpr std::int64_t registry_schema_version = 1;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

} // namespace

Registry Registry::load(const fs::path& file) {
    Registry registry;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return registry;
    }
    const std::string name = utf8(file);
    std::istringstream stream(platform::read_text_file(file, 4u << 20));
    auto parsed = toml::try_parse(stream, name);
    if (parsed.is_err()) {
        std::string message;
        for (const toml::error_info& e : parsed.unwrap_err()) {
            const std::size_t line =
                e.locations().empty() ? 0 : e.locations().front().first.first_line_number();
            message += (message.empty() ? "" : "\n") + name + ":" + std::to_string(line) + ": " + e.title();
        }
        throw Error(ErrorCode::registry_parse, message);
    }
    const toml::value root = std::move(parsed.unwrap());
    const auto fail = [&](const toml::value& at, const std::string& message) {
        throw Error(ErrorCode::registry_parse,
                    name + ":" + std::to_string(at.location().first_line_number()) + ": " + message);
    };
    if (!root.contains("install")) {
        return registry;
    }
    const toml::value& list = root.at("install");
    if (!list.is_array()) {
        fail(list, "install must be an array of tables ([[install]])");
    }
    for (const toml::value& t : list.as_array()) {
        if (!t.is_table()) {
            fail(t, "every install must be a table ([[install]])");
        }
        const auto text = [&](const char* key, bool required) -> std::string {
            if (!t.contains(key)) {
                if (required) {
                    fail(t, std::string("an install needs '") + key + "'");
                }
                return {};
            }
            if (!t.at(key).is_string()) {
                fail(t.at(key), std::string("'") + key + "' must be a string");
            }
            return t.at(key).as_string();
        };
        Install install;
        install.profile_id = text("profile", true);
        install.iso = from_utf8(text("iso", false));
        install.module_dir = from_utf8(text("module_dir", true));
        install.manifest_sha1 = text("manifest_sha1", false);
        install.built_at = text("built_at", false);
        install.host_version = text("host_version", false);
        if (install.profile_id.empty()) {
            fail(t, "'profile' must not be empty");
        }
        registry.upsert(std::move(install));
    }
    return registry;
}

void Registry::save(const fs::path& file) const {
    toml::ordered_value root(toml::ordered_table{});
    root["schema_version"] = registry_schema_version;
    toml::ordered_value installs(toml::ordered_array{});
    for (const Install& install : installs_) {
        toml::ordered_value t(toml::ordered_table{});
        t["profile"] = install.profile_id;
        t["iso"] = utf8(install.iso);
        t["module_dir"] = utf8(install.module_dir);
        t["manifest_sha1"] = install.manifest_sha1;
        t["built_at"] = install.built_at;
        t["host_version"] = install.host_version;
        installs.push_back(std::move(t));
    }
    installs.as_array_fmt().fmt = toml::array_format::array_of_tables;
    root["install"] = std::move(installs);
    platform::write_file_atomic(file,
                                "# DQ8-Native: the games set up on this machine (written by `dq8ctl setup` "
                                "and the launcher).\n" +
                                    toml::format(root));
}

const Install* Registry::find(std::string_view profile_id) const noexcept {
    for (const Install& install : installs_) {
        if (install.profile_id == profile_id) {
            return &install;
        }
    }
    return nullptr;
}

void Registry::upsert(Install install) {
    for (Install& existing : installs_) {
        if (existing.profile_id == install.profile_id) {
            existing = std::move(install);
            return;
        }
    }
    installs_.push_back(std::move(install));
}

fs::path default_file() {
    return platform::user_data_dir() / "installs.toml";
}

} // namespace dq8::core::registry
