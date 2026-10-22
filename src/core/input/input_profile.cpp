// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Input profiles (dq8/core/input_profile.h): parsing, validation and writing. M5 session C.
#include "dq8/core/input_profile.h"

#include "dq8/core/error.h"
#include "platform/files.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <toml.hpp>

namespace dq8::core::input {

namespace {

// Collects "<file>:<line>: message" warnings for one file.
class Problems {
  public:
    Problems(std::string_view origin, std::vector<std::string>& out) : origin_(origin), out_(out) {}
    void add(const toml::value& at, const std::string& message) const {
        out_.push_back(origin_ + ":" + std::to_string(at.location().first_line_number()) + ": " + message);
    }

  private:
    std::string origin_;
    std::vector<std::string>& out_;
};

bool is_guid(std::string_view s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool is_source_name(const toml::value& item) {
    return item.is_string() && !item.as_string().empty() && item.as_string().size() <= 64 &&
           std::none_of(item.as_string().begin(), item.as_string().end(),
                        [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

// The sources of one control: an array of source names (non-empty strings of up to 64 printable characters).
// Not an array: nothing (the caller keeps what it had); an invalid entry is left out.
std::optional<std::vector<std::string>> sources(const Problems& problems, const toml::value& v,
                                                const std::string& label) {
    if (!v.is_array()) {
        problems.add(v, label + " is not an array of source names, e.g. [\"K\"]; ignored");
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (const toml::value& item : v.as_array()) {
        if (!is_source_name(item)) {
            problems.add(item, label + ": an entry that is not a source name (1 to 64 printable characters); "
                                       "left out");
            continue;
        }
        if (std::find(out.begin(), out.end(), item.as_string()) == out.end()) {
            out.push_back(item.as_string());
        }
    }
    return out;
}

// [keyboard] or [gamepad]: the controls it lists replace the built-in ones.
void read_bindings(const Problems& problems, const toml::value& root, const char* key, Bindings& bindings) {
    if (!root.contains(key)) {
        return;
    }
    const toml::value& table = root.at(key);
    if (!table.is_table()) {
        problems.add(table, std::string("[") + key + "] is not a table; the built-in bindings apply");
        return;
    }
    for (const auto& [name, value] : table.as_table()) {
        const auto control = control_from_name(name);
        if (!control) {
            problems.add(value, std::string("[") + key + "]: '" + name + "' is not a pad control; ignored");
            continue;
        }
        if (auto list = sources(problems, value, std::string("[") + key + "]." + name)) {
            bindings[*control] = std::move(*list);
        }
    }
}

// One [devices."<guid>"] table, or nothing when it cannot be used.
std::optional<DeviceProfile> read_device(const Problems& problems, const std::string& guid,
                                         const toml::value& table) {
    const std::string label = "[devices.\"" + guid + "\"]";
    if (!is_guid(guid)) {
        problems.add(table,
                     "[devices]: '" + guid + "' is not a gamepad GUID (32 lowercase hex digits); ignored");
        return std::nullopt;
    }
    if (!table.is_table()) {
        problems.add(table, label + " is not a table; ignored");
        return std::nullopt;
    }
    DeviceProfile d;
    d.guid = guid;
    for (const auto& [key, value] : table.as_table()) {
        if (key == "name") {
            if (value.is_string()) {
                d.name = value.as_string();
            } else {
                problems.add(value, label + ".name is not a string; ignored");
            }
        } else if (key == "port") {
            // Counted as players count them: 1 or 2.
            if (value.is_integer() && value.as_integer() >= 1 && value.as_integer() <= 2) {
                d.port = static_cast<unsigned>(value.as_integer() - 1);
            } else {
                problems.add(value, label + ".port is not 1 or 2; ignored");
            }
        } else if (const auto control = control_from_name(key)) {
            if (auto list = sources(problems, value, label + "." + key)) {
                d.overrides[static_cast<std::size_t>(*control)] = std::move(*list);
            }
        } else {
            problems.add(value, label + ": '" + key + "' is not a pad control, name or port; ignored");
        }
    }
    return d;
}

// A TOML basic string: quotes and backslashes escaped, control characters as \uXXXX.
std::string toml_string(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20 || u == 0x7F) {
            out += "\\u00";
            out += hex[u >> 4];
            out += hex[u & 0xF];
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

void write_sources(std::ostringstream& out, std::string_view name, const std::vector<std::string>& list) {
    out << name << " = [";
    for (std::size_t i = 0; i < list.size(); ++i) {
        out << (i == 0 ? "" : ", ") << toml_string(list[i]);
    }
    out << "]\n";
}

} // namespace

std::string_view control_name(Control control) noexcept {
    const auto i = static_cast<std::size_t>(control);
    return i < control_count ? control_names[i] : std::string_view("?");
}

std::optional<Control> control_from_name(std::string_view name) noexcept {
    for (std::size_t i = 0; i < control_count; ++i) {
        if (control_names[i] == name) {
            return control_at(i);
        }
    }
    return std::nullopt;
}

const DeviceProfile* Profile::device(std::string_view guid) const noexcept {
    for (const DeviceProfile& d : devices) {
        if (d.guid == guid) {
            return &d;
        }
    }
    return nullptr;
}

Bindings Profile::gamepad_for(std::string_view guid) const {
    Bindings out = gamepad;
    if (const DeviceProfile* d = device(guid)) {
        for (std::size_t i = 0; i < control_count; ++i) {
            if (d->overrides[i]) {
                out.sources[i] = *d->overrides[i];
            }
        }
    }
    return out;
}

Profile default_profile() {
    Profile p;
    Bindings& k = p.keyboard;
    k[Control::up] = {"Up"};
    k[Control::down] = {"Down"};
    k[Control::left] = {"Left"};
    k[Control::right] = {"Right"};
    k[Control::triangle] = {"I"};
    k[Control::circle] = {"L"};
    k[Control::cross] = {"K"};
    k[Control::square] = {"J"};
    k[Control::l1] = {"Q"};
    k[Control::r1] = {"E"};
    k[Control::l2] = {"1"};
    k[Control::r2] = {"3"};
    k[Control::l3] = {"2"};
    k[Control::r3] = {"4"};
    k[Control::start] = {"Return"};
    k[Control::select] = {"Backspace"};
    k[Control::left_stick_up] = {"W"};
    k[Control::left_stick_down] = {"S"};
    k[Control::left_stick_left] = {"A"};
    k[Control::left_stick_right] = {"D"};
    k[Control::right_stick_up] = {"T"};
    k[Control::right_stick_down] = {"G"};
    k[Control::right_stick_left] = {"F"};
    k[Control::right_stick_right] = {"H"};

    Bindings& g = p.gamepad;
    g[Control::up] = {"dpup"};
    g[Control::down] = {"dpdown"};
    g[Control::left] = {"dpleft"};
    g[Control::right] = {"dpright"};
    g[Control::triangle] = {"y"};
    g[Control::circle] = {"b"};
    g[Control::cross] = {"a"};
    g[Control::square] = {"x"};
    g[Control::l1] = {"leftshoulder"};
    g[Control::r1] = {"rightshoulder"};
    g[Control::l2] = {"lefttrigger"};
    g[Control::r2] = {"righttrigger"};
    g[Control::l3] = {"leftstick"};
    g[Control::r3] = {"rightstick"};
    g[Control::start] = {"start"};
    g[Control::select] = {"back"};
    g[Control::left_stick_up] = {"lefty-"};
    g[Control::left_stick_down] = {"lefty+"};
    g[Control::left_stick_left] = {"leftx-"};
    g[Control::left_stick_right] = {"leftx+"};
    g[Control::right_stick_up] = {"righty-"};
    g[Control::right_stick_down] = {"righty+"};
    g[Control::right_stick_left] = {"rightx-"};
    g[Control::right_stick_right] = {"rightx+"};
    return p;
}

Profile parse_profile(std::string_view text, std::string_view origin, std::vector<std::string>& warnings) {
    std::istringstream stream{std::string(text)};
    auto parsed = toml::try_parse(stream, std::string(origin));
    if (parsed.is_err()) {
        std::string message;
        for (const toml::error_info& e : parsed.unwrap_err()) {
            const std::size_t line =
                e.locations().empty() ? 0 : e.locations().front().first.first_line_number();
            message += (message.empty() ? "" : "\n") + std::string(origin) + ":" + std::to_string(line) +
                       ": " + e.title();
        }
        throw Error(ErrorCode::settings_parse, message);
    }
    const toml::value root = std::move(parsed.unwrap());
    const Problems problems(origin, warnings);
    if (!root.contains("schema_version") || !root.at("schema_version").is_integer()) {
        problems.add(root, "no 'schema_version = 1'; read as version 1");
    } else if (root.at("schema_version").as_integer() != profile_schema_version) {
        problems.add(root.at("schema_version"),
                     "schema_version " + std::to_string(root.at("schema_version").as_integer()) +
                         " is not the version this build reads (" + std::to_string(profile_schema_version) +
                         "); read as version 1");
    }
    for (const auto& [key, value] : root.as_table()) {
        if (key != "schema_version" && key != "keyboard" && key != "gamepad" && key != "devices") {
            problems.add(value,
                         "unknown key '" + key + "' (schema_version, keyboard, gamepad, devices); ignored");
        }
    }

    Profile p = default_profile();
    read_bindings(problems, root, "keyboard", p.keyboard);
    read_bindings(problems, root, "gamepad", p.gamepad);
    if (root.contains("devices")) {
        const toml::value& devices = root.at("devices");
        if (!devices.is_table()) {
            problems.add(devices, "[devices] is not a table of [devices.\"<guid>\"] tables; ignored");
        } else {
            for (const auto& [guid, table] : devices.as_table()) {
                if (auto d = read_device(problems, guid, table)) {
                    p.devices.push_back(std::move(*d));
                }
            }
            // The file's tables are unordered; keep a stable order.
            std::sort(p.devices.begin(), p.devices.end(),
                      [](const DeviceProfile& x, const DeviceProfile& y) { return x.guid < y.guid; });
        }
    }
    return p;
}

Profile load_profile(const std::filesystem::path& file, std::vector<std::string>& warnings) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        throw Error(ErrorCode::io_open, "no input profile at '" + file.string() + "'");
    }
    return parse_profile(platform::read_text_file(file, 1u << 20), file.string(), warnings);
}

std::string format_profile(const Profile& profile) {
    std::ostringstream out;
    out << "# DQ8-Native input profile (Docs/formats/input-profile.md). Keys are SDL3 scancode names; "
           "gamepad\n"
           "# sources are SDL3 gamepad button and axis names (\"a\" is the south face button), an axis with "
           "a\n"
           "# sign for one half. An empty list unbinds a control.\n"
           "schema_version = "
        << profile_schema_version << "\n";
    const auto table = [&out](std::string_view name, const Bindings& b) {
        out << "\n[" << name << "]\n";
        for (std::size_t i = 0; i < control_count; ++i) {
            write_sources(out, control_names[i], b.sources[i]);
        }
    };
    table("keyboard", profile.keyboard);
    table("gamepad", profile.gamepad);
    std::vector<const DeviceProfile*> devices;
    for (const DeviceProfile& d : profile.devices) {
        devices.push_back(&d);
    }
    std::sort(devices.begin(), devices.end(),
              [](const DeviceProfile* a, const DeviceProfile* b) { return a->guid < b->guid; });
    for (const DeviceProfile* d : devices) {
        out << "\n[devices." << toml_string(d->guid) << "]\n";
        if (!d->name.empty()) {
            out << "name = " << toml_string(d->name) << "\n";
        }
        if (d->port) {
            out << "port = " << *d->port + 1 << "\n";
        }
        for (std::size_t i = 0; i < control_count; ++i) {
            if (d->overrides[i]) {
                write_sources(out, control_names[i], *d->overrides[i]);
            }
        }
    }
    return out.str();
}

void save_profile(const std::filesystem::path& file, const Profile& profile) {
    platform::write_file_atomic(file, format_profile(profile));
}

bool valid_profile_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
               c == '-';
    });
}

std::filesystem::path profile_file(const std::filesystem::path& config_dir, std::string_view name) {
    return config_dir / "input" / (std::string(name) + ".toml");
}

} // namespace dq8::core::input
