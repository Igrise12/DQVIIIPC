// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

// Pieces shared by dq8ctl's command files: exit statuses, the JSON writer, argument parsing and the lookup of
// data directories relative to the executable. See main.cpp for the contract of --json and the exit statuses.
#include "dq8/core/error.h"
#include "dq8/core/hash.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dq8::cli {

namespace fs = std::filesystem;
using dq8::core::Error;
using dq8::core::ErrorCode;

inline constexpr int exit_ok = 0;
inline constexpr int exit_mismatch = 1;
inline constexpr int exit_error = 2;

// Minimal JSON writer: objects, arrays, strings, integers, booleans. Enough for the CLI's output.
class Json {
  public:
    void begin_object() {
        separate();
        out_ += '{';
        first_.push_back(true);
    }
    void end_object() {
        out_ += '}';
        first_.pop_back();
    }
    void begin_array() {
        separate();
        out_ += '[';
        first_.push_back(true);
    }
    void end_array() {
        out_ += ']';
        first_.pop_back();
    }
    void key(std::string_view k) {
        separate();
        quote(k);
        out_ += ':';
        pending_key_ = true;
    }
    void value(std::string_view s) {
        separate();
        quote(s);
    }
    void value(const char* s) { value(std::string_view(s)); }
    void value(const std::string& s) { value(std::string_view(s)); }
    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    void value(T n) {
        separate();
        out_ += std::to_string(n);
    }
    void value(bool b) {
        separate();
        out_ += b ? "true" : "false";
    }
    void null() {
        separate();
        out_ += "null";
    }
    template <typename T> void member(std::string_view k, const T& v) {
        key(k);
        value(v);
    }
    const std::string& str() const { return out_; }

  private:
    void separate() {
        if (pending_key_) {
            pending_key_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ += ',';
            }
            first_.back() = false;
        }
    }
    void quote(std::string_view s) {
        out_ += '"';
        for (const char c : s) {
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out_ += buf;
                } else {
                    out_ += c;
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool pending_key_ = false;
};

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;             // --name VALUE
    std::set<std::string> flags;                           // --name
    std::map<std::string, std::vector<std::string>> lists; // --name VALUE, repeatable
    std::vector<std::string> passthrough;                  // everything after a lone "--" (dq8ctl run)
};

inline const std::set<std::string> value_options{
    "regions",    "out",    "maps",   "work",      "units",           "jobs",
    "ps2-recomp", "cmake",  "boot",   "generator", "module-template", "abi-include-dir",
    "profile-id", "events", "config", "profile",   "registry",        "game",
    "smoke-input"};
inline const std::set<std::string> list_options{"unit-elf"}; // may repeat; values collect in order
inline const std::set<std::string> flag_options{"json",          "force",  "emit-profile", "no-verify-hashes",
                                                "verbose",       "no-pch", "debug-info",   "no-smoke",
                                                "allow-download"};

inline bool parse_args(int argc, char** argv, Args& args, std::string& error) {
    args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            args.passthrough.assign(argv + i + 1, argv + argc);
            break;
        }
        if (arg.size() > 2 && arg.compare(0, 2, "--") == 0) {
            std::string name = arg.substr(2);
            std::string value;
            bool has_value = false;
            const std::size_t eq = name.find('=');
            if (eq != std::string::npos) {
                value = name.substr(eq + 1);
                name.erase(eq);
                has_value = true;
            }
            if (value_options.count(name) != 0 || list_options.count(name) != 0) {
                if (!has_value) {
                    if (i + 1 >= argc) {
                        error = "--" + name + " needs a value";
                        return false;
                    }
                    value = argv[++i];
                }
                if (list_options.count(name) != 0) {
                    args.lists[name].push_back(value);
                } else {
                    args.values[name] = value;
                }
            } else if (flag_options.count(name) != 0 && !has_value) {
                args.flags.insert(name);
            } else {
                error = "unknown option '" + arg + "'";
                return false;
            }
        } else {
            args.positional.push_back(arg);
        }
    }
    return true;
}

inline std::string hex32(std::uint32_t v) {
    return "0x" + dq8::core::to_hex(v);
}

inline fs::path executable_dir(const char* argv0) {
    std::error_code ec;
#ifdef __linux__
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path();
    }
#elif defined(_WIN32)
    // The CRT's full path of this executable: argv[0] may be a bare name typed at a prompt (M5 session E).
    char* self = nullptr;
    if (_get_pgmptr(&self) == 0 && self != nullptr && self[0] != '\0') {
        return fs::path(self).parent_path();
    }
#endif
    const fs::path p = fs::canonical(argv0, ec);
    return ec ? fs::path() : p.parent_path();
}

// --regions, else data/regions under the current directory, under an ancestor of the executable (the build
// tree) or share/dq8/regions beside it (an install).
inline fs::path regions_dir(const Args& args, const char* argv0) {
    const auto it = args.values.find("regions");
    if (it != args.values.end()) {
        return it->second;
    }
    std::vector<fs::path> candidates{fs::path("data") / "regions"};
    fs::path dir = executable_dir(argv0);
    for (int up = 0; up < 8 && !dir.empty(); ++up) { // 8: a multi-config build adds Release/ or Debug/
        candidates.push_back(dir / "data" / "regions");
        candidates.push_back(dir / "share" / "dq8" / "regions");
        dir = dir.parent_path();
    }
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::is_directory(c, ec)) {
            return c;
        }
    }
    throw Error(ErrorCode::io_open, "no region profile directory found; pass --regions DIR");
}

inline void print_warnings(const std::vector<std::string>& warnings, std::FILE* out) {
    for (const std::string& w : warnings) {
        std::fprintf(out, "warning: %s\n", w.c_str());
    }
}

// Prints the usage text; returns exit_ok for stdout and exit_error for stderr.
int usage(std::FILE* out);

// `dq8ctl recomp prepare|generate|build|all ...` and `dq8ctl make-unit-elf ...` (recomp_commands.cpp).
int run_recomp(const Args& args, const char* argv0);
int run_make_unit_elf(const Args& args, const char* argv0);

// M5: `dq8ctl setup`, `run` and `diagnose` (setup_commands.cpp), `dq8ctl settings` (settings_commands.cpp).
int run_setup(const Args& args, const char* argv0);
int run_game(const Args& args, const char* argv0);
int run_diagnose(const Args& args, const char* argv0);
int run_settings(const Args& args, const char* argv0);

// The {"ok": false, "error": {...}} object (--json) or the "dq8ctl <command>: ..." line; returns the exit
// status.
int report_error(const Args& args, const Error& e);

// UTF-8 text of a path.
inline std::string path_text(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace dq8::cli
