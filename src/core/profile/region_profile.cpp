// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/region_profile.h"

#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/mwo3.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <toml.hpp>

namespace dq8::core {

namespace {

bool is_hex(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// Pulls typed fields out of a parsed document, reporting every problem as "<file>:<line>: ...".
class Reader {
  public:
    explicit Reader(std::string file) : file_(std::move(file)) {}

    [[noreturn]] void fail(const toml::value& at, const std::string& message) const {
        throw Error(ErrorCode::profile_schema,
                    file_ + ":" + std::to_string(at.location().first_line_number()) + ": " + message);
    }

    const toml::value& table(const toml::value& parent, const char* key, const std::string& label) const {
        if (!parent.contains(key)) {
            fail(parent, "missing table [" + label + "]");
        }
        const toml::value& v = parent.at(key);
        if (!v.is_table()) {
            fail(v, "[" + label + "] must be a table");
        }
        return v;
    }

    const toml::value* optional(const toml::value& t, const char* key) const {
        return t.contains(key) ? &t.at(key) : nullptr;
    }

    std::string string(const toml::value& t, const char* key, const std::string& label) const {
        const toml::value* v = optional(t, key);
        if (v == nullptr) {
            fail(t, "[" + label + "] is missing '" + key + "'");
        }
        if (!v->is_string()) {
            fail(*v, "[" + label + "]." + key + " must be a string");
        }
        const std::string s = v->as_string();
        if (s.empty()) {
            fail(*v, "[" + label + "]." + key + " must not be empty");
        }
        return s;
    }

    std::int64_t integer(const toml::value& t, const char* key, const std::string& label, std::int64_t min,
                         std::int64_t max) const {
        const toml::value* v = optional(t, key);
        if (v == nullptr) {
            fail(t, "[" + label + "] is missing '" + key + "'");
        }
        if (!v->is_integer()) {
            fail(*v, "[" + label + "]." + key + " must be an integer");
        }
        const std::int64_t n = v->as_integer();
        if (n < min || n > max) {
            fail(*v, "[" + label + "]." + key + " is out of range");
        }
        return n;
    }

    std::uint32_t u32(const toml::value& t, const char* key, const std::string& label) const {
        return static_cast<std::uint32_t>(
            integer(t, key, label, 0, std::numeric_limits<std::uint32_t>::max()));
    }

    std::uint64_t u64(const toml::value& t, const char* key, const std::string& label) const {
        return static_cast<std::uint64_t>(
            integer(t, key, label, 0, std::numeric_limits<std::int64_t>::max()));
    }

    // A hash string of `digits` hex characters. When `required` is false the key may be absent or empty,
    // meaning "not recorded yet"; the result is then empty.
    std::string hex(const toml::value& t, const char* key, const std::string& label, std::size_t digits,
                    bool required) const {
        const toml::value* v = optional(t, key);
        if (v == nullptr) {
            if (required) {
                fail(t, "[" + label + "] is missing '" + key + "'");
            }
            return {};
        }
        if (!v->is_string()) {
            fail(*v,
                 "[" + label + "]." + key + " must be a string of " + std::to_string(digits) + " hex digits");
        }
        const std::string s = v->as_string();
        if (s.empty()) {
            if (required) {
                fail(*v, "[" + label + "]." + key + " must not be empty");
            }
            return {};
        }
        if (s.size() != digits || !is_hex(s)) {
            fail(*v, "[" + label + "]." + key + " must be " + std::to_string(digits) + " hex digits");
        }
        return normalize_hex(s);
    }

    // An array of non-empty strings.
    std::vector<std::string> strings(const toml::value& t, const char* key, const std::string& label) const {
        const toml::value* v = optional(t, key);
        if (v == nullptr) {
            fail(t, "[" + label + "] is missing '" + key + "'");
        }
        if (!v->is_array()) {
            fail(*v, "[" + label + "]." + key + " must be an array of strings");
        }
        std::vector<std::string> out;
        for (const toml::value& item : v->as_array()) {
            if (!item.is_string() || item.as_string().empty()) {
                fail(item, "[" + label + "]." + key + " entries must be non-empty strings");
            }
            out.push_back(item.as_string());
        }
        return out;
    }

    // A disc path: relative, '/'-separated, no empty or '.'/'..' components, no version suffix.
    std::string disc_path(const toml::value& t, const char* key, const std::string& label,
                          bool allow_directory = false) const {
        const toml::value* v = optional(t, key);
        const std::string s = string(t, key, label);
        bool directory = false;
        std::string rest = s;
        if (allow_directory && rest.size() > 1 && rest.back() == '/') {
            directory = true;
            rest.pop_back();
        }
        std::string component;
        auto check = [&](const std::string& c) {
            if (c.empty() || c == "." || c == ".." || c.find(';') != std::string::npos) {
                fail(*v, "[" + label + "]." + key + " '" + s + "' is not a plain relative disc path");
            }
        };
        for (const char c : rest) {
            if (c == '/') {
                check(component);
                component.clear();
            } else if (c == '\\') {
                fail(*v, "[" + label + "]." + key + " '" + s + "' must use '/' separators");
            } else {
                component.push_back(c);
            }
        }
        check(component);
        return directory ? rest + "/" : rest;
    }

  private:
    std::string file_;
};

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

// True when `path` is one of the not_extracted entries or lies under one of its directory entries.
bool is_not_extracted(const std::string& path, const std::vector<std::string>& not_extracted) {
    const std::string p = upper(path);
    for (const std::string& n : not_extracted) {
        const std::string u = upper(n);
        if (!u.empty() && u.back() == '/') {
            if (p.size() > u.size() && p.compare(0, u.size(), u) == 0) {
                return true;
            }
        } else if (p == u) {
            return true;
        }
    }
    return false;
}

} // namespace

RegionProfile load_region_profile(const std::filesystem::path& file) {
    const std::string name = file.string();
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw Error(ErrorCode::io_open, "cannot open region profile '" + name + "'");
    }
    auto parsed = toml::try_parse(stream, name);
    if (parsed.is_err()) {
        // "<file>:<line>: <title>" first, then toml11's annotated excerpt.
        std::string message;
        for (const toml::error_info& e : parsed.unwrap_err()) {
            if (!message.empty()) {
                message += '\n';
            }
            const std::size_t line =
                e.locations().empty() ? 0 : e.locations().front().first.first_line_number();
            message += name + ":" + std::to_string(line) + ": " + e.title() + "\n" + toml::format_error(e);
        }
        throw Error(ErrorCode::profile_parse, message);
    }
    const toml::value root = std::move(parsed.unwrap());
    const Reader r(name);
    RegionProfile p;
    p.source = file;

    const std::int64_t version = r.integer(root, "schema_version", "(root)", 1, 1000);
    if (version != region_profile_schema_version) {
        r.fail(root.at("schema_version"), "schema_version " + std::to_string(version) +
                                              " is not supported (this build reads " +
                                              std::to_string(region_profile_schema_version) + ")");
    }
    p.schema_version = static_cast<int>(version);

    const toml::value& profile = r.table(root, "profile", "profile");
    p.id = r.string(profile, "id", "profile");
    p.serial = r.string(profile, "serial", "profile");
    p.title = r.string(profile, "title", "profile");
    p.region = r.string(profile, "region", "profile");
    p.video_mode = r.string(profile, "video_mode", "profile");
    for (const char c : p.id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) {
            r.fail(profile.at("id"), "[profile].id may only contain letters, digits, '_' and '-'");
        }
    }

    const toml::value& image = r.table(root, "image", "image");
    p.image_size = r.u64(image, "size", "image");
    p.sector_size = r.u32(image, "sector_size", "image");
    if (p.sector_size != 2048) {
        r.fail(image.at("sector_size"), "[image].sector_size must be 2048");
    }

    const toml::value& boot = r.table(root, "boot", "boot");
    p.boot.path = r.disc_path(boot, "path", "boot");
    p.boot.size = r.u64(boot, "size", "boot");
    p.boot.sha1 = r.hex(boot, "sha1", "boot", 40, true);
    p.boot.crc32 = r.hex(boot, "crc32", "boot", 8, true);
    p.boot.entry = r.u32(boot, "entry", "boot");
    p.boot.load_address = r.u32(boot, "load_address", "boot");
    p.boot.main_memsz = r.u32(boot, "main_memsz", "boot");

    const toml::value& window = r.table(root, "overlay_window", "overlay_window");
    RegionProfile::OverlayWindow& w = p.overlay_window;
    w.base = r.u32(window, "base", "overlay_window");
    w.end = r.u32(window, "end", "overlay_window");
    w.container = r.string(window, "container", "overlay_window");
    w.header_size = r.u32(window, "header_size", "overlay_window");
    w.code_offset = r.u32(window, "code_offset", "overlay_window");
    if (w.end <= w.base) {
        r.fail(window, "[overlay_window].end must be greater than base");
    }
    if (w.container == "MWo3" && w.header_size != mwo3_header_size) {
        r.fail(window.at("header_size"), "[overlay_window].header_size must be " +
                                             std::to_string(mwo3_header_size) + " for an MWo3 container");
    }
    if (w.code_offset != w.header_size) {
        r.fail(window.at("code_offset"), "[overlay_window].code_offset must equal header_size (the text "
                                         "follows the resident header)");
    }
    if (w.header_size >= w.end - w.base) {
        r.fail(window.at("header_size"), "[overlay_window].header_size does not fit the window");
    }
    // Loader facts (spike S3): code and data addresses inside the main program, 4-byte aligned.
    const std::uint64_t main_begin = p.boot.load_address;
    const std::uint64_t main_end = main_begin + p.boot.main_memsz;
    auto main_address = [&](const char* key) {
        const std::uint32_t a = r.u32(window, key, "overlay_window");
        if (a % 4 != 0) {
            r.fail(window.at(key), std::string("[overlay_window].") + key + " must be 4-byte aligned");
        }
        if (a < main_begin || a >= main_end) {
            r.fail(window.at(key), std::string("[overlay_window].") + key +
                                       " must lie inside the main program ([boot].load_address .. "
                                       "load_address + main_memsz)");
        }
        return a;
    };
    w.window_pointer = main_address("window_pointer");
    w.loader = main_address("loader");
    w.loader_hook = main_address("loader_hook");
    w.load_by_index = main_address("load_by_index");
    w.name_table = main_address("name_table");
    w.lsn_cache = main_address("lsn_cache");
    w.ctor_walker = main_address("ctor_walker");
    w.flush_cache_stub = main_address("flush_cache_stub");
    w.mode_variable = main_address("mode_variable");
    w.name_table_order = r.strings(window, "name_table_order", "overlay_window");
    {
        std::set<std::string> seen;
        for (const std::string& n : w.name_table_order) {
            if (!seen.insert(n).second) {
                r.fail(window.at("name_table_order"),
                       "[overlay_window].name_table_order lists '" + n + "' twice");
            }
        }
    }

    if (const toml::value* list = r.optional(root, "not_extracted")) {
        if (!list->is_table()) {
            r.fail(*list, "[not_extracted] must be a table");
        }
        const toml::value* paths = r.optional(*list, "paths");
        if (paths == nullptr || !paths->is_array()) {
            r.fail(*list, "[not_extracted] must hold a 'paths' array");
        }
        for (const toml::value& item : paths->as_array()) {
            if (!item.is_string() || item.as_string().empty()) {
                r.fail(item, "[not_extracted].paths entries must be non-empty strings");
            }
            toml::value holder = toml::table{{"path", item}};
            p.not_extracted.push_back(r.disc_path(holder, "path", "not_extracted", true));
        }
    }

    std::set<std::string> seen_paths;
    auto note_path = [&](const toml::value& at, const std::string& path, const std::string& label) {
        if (!seen_paths.insert(upper(path)).second) {
            r.fail(at, "[" + label + "] lists '" + path + "' twice");
        }
        if (is_not_extracted(path, p.not_extracted)) {
            r.fail(at, "[" + label + "] lists '" + path + "', which [not_extracted] excludes");
        }
    };
    note_path(boot, p.boot.path, "boot");

    if (const toml::value* overlays = r.optional(root, "overlays")) {
        if (!overlays->is_array()) {
            r.fail(*overlays, "[[overlays]] must be an array of tables");
        }
        std::set<std::uint32_t> ids;
        std::set<std::uint32_t> indexes;
        std::set<std::uint32_t> segments;
        std::set<std::string> names;
        for (const toml::value& t : overlays->as_array()) {
            if (!t.is_table()) {
                r.fail(t, "[[overlays]] entries must be tables");
            }
            ProfileOverlay o;
            o.name = r.string(t, "name", "overlays");
            o.id = r.u32(t, "id", "overlays");
            o.index = r.u32(t, "index", "overlays");
            o.segment_index = r.u32(t, "segment_index", "overlays");
            o.path = r.disc_path(t, "path", "overlays");
            o.segment_memsz = r.u32(t, "segment_memsz", "overlays");
            o.text_size = r.u32(t, "text_size", "overlays");
            o.data_size = r.u32(t, "data_size", "overlays");
            o.bss_size = r.u32(t, "bss_size", "overlays");
            o.table_entries = r.u32(t, "table_entries", "overlays");
            o.size = r.u64(t, "size", "overlays");
            o.sha1 = r.hex(t, "sha1", "overlays", 40, false);
            o.crc32 = r.hex(t, "crc32", "overlays", 8, false);
            if (!names.insert(o.name).second) {
                r.fail(t.at("name"), "[[overlays]].name '" + o.name + "' is used twice");
            }
            if (o.id == 0 || o.id > mwo3_max_overlay_id || !ids.insert(o.id).second) {
                r.fail(t.at("id"), "[[overlays]].id must be unique and between 1 and " +
                                       std::to_string(mwo3_max_overlay_id));
            }
            if (o.index >= w.name_table_order.size() || w.name_table_order[o.index] != o.name) {
                r.fail(t.at("index"), "[[overlays]].index " + std::to_string(o.index) +
                                          " is not the position of '" + o.name +
                                          "' in [overlay_window].name_table_order");
            }
            if (!indexes.insert(o.index).second) {
                r.fail(t.at("index"), "[[overlays]].index must be unique");
            }
            if (o.segment_index == 0 || !segments.insert(o.segment_index).second) {
                r.fail(t.at("segment_index"),
                       "[[overlays]].segment_index must be unique and at least 1 (0 is the main image)");
            }
            if (o.segment_memsz > w.end - w.base) {
                r.fail(t.at("segment_memsz"), "[[overlays]].segment_memsz does not fit the overlay window");
            }
            const std::uint64_t file_size =
                static_cast<std::uint64_t>(w.header_size) + o.text_size + o.data_size;
            if (file_size + o.bss_size != o.segment_memsz) {
                r.fail(t.at("segment_memsz"), "[[overlays]] header_size + text_size + data_size + bss_size "
                                              "must equal segment_memsz");
            }
            if (o.size != 0 && file_size != o.size) {
                r.fail(t.at("size"), "[[overlays]].size must equal header_size + text_size + data_size");
            }
            if (static_cast<std::uint64_t>(o.table_entries) * 4 > o.data_size) {
                r.fail(t.at("table_entries"), "[[overlays]].table_entries do not fit in data_size");
            }
            note_path(t, o.path, "overlays");
            p.overlays.push_back(std::move(o));
        }
    }
    if (p.overlays.size() != w.name_table_order.size()) {
        r.fail(window.at("name_table_order"),
               "[overlay_window].name_table_order has " + std::to_string(w.name_table_order.size()) +
                   " entries but there are " + std::to_string(p.overlays.size()) + " [[overlays]]");
    }

    if (const toml::value* files = r.optional(root, "files")) {
        if (!files->is_array()) {
            r.fail(*files, "[[files]] must be an array of tables");
        }
        static const std::set<std::string> roles{"iop_module", "iop_image", "system_cnf", "other"};
        for (const toml::value& t : files->as_array()) {
            if (!t.is_table()) {
                r.fail(t, "[[files]] entries must be tables");
            }
            ProfileFile f;
            f.path = r.disc_path(t, "path", "files");
            f.role = r.string(t, "role", "files");
            if (roles.count(f.role) == 0) {
                r.fail(t.at("role"), "[[files]].role '" + f.role +
                                         "' is not one of iop_module, iop_image, system_cnf, other");
            }
            f.size = r.u64(t, "size", "files");
            f.sha1 = r.hex(t, "sha1", "files", 40, false);
            f.crc32 = r.hex(t, "crc32", "files", 8, false);
            note_path(t, f.path, "files");
            p.files.push_back(std::move(f));
        }
    }
    return p;
}

std::vector<RegionProfile> load_region_profiles(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        throw Error(ErrorCode::io_open, "region profile directory '" + dir.string() + "' does not exist");
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".toml") {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        throw Error(ErrorCode::io_open, "cannot list '" + dir.string() + "': " + ec.message());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        throw Error(ErrorCode::io_open, "no region profiles (*.toml) in '" + dir.string() + "'");
    }
    std::vector<RegionProfile> profiles;
    for (const auto& file : files) {
        RegionProfile p = load_region_profile(file);
        for (const RegionProfile& other : profiles) {
            if (other.id == p.id) {
                throw Error(ErrorCode::profile_conflict, "'" + file.string() + "' and '" +
                                                             other.source.string() + "' both have id '" +
                                                             p.id + "'");
            }
            if (other.boot.sha1 == p.boot.sha1) {
                throw Error(ErrorCode::profile_conflict, "'" + file.string() + "' and '" +
                                                             other.source.string() +
                                                             "' record the same boot file hash");
            }
        }
        profiles.push_back(std::move(p));
    }
    return profiles;
}

const RegionProfile* find_profile_by_boot_sha1(std::span<const RegionProfile> profiles,
                                               std::string_view sha1_hex) {
    const std::string wanted = normalize_hex(sha1_hex);
    for (const RegionProfile& p : profiles) {
        if (p.boot.sha1 == wanted) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace dq8::core
