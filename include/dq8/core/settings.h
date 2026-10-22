// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Settings (architecture.md §9, ADR-0006): every user-facing setting is declared once in a schema
// (config/settings/schema.toml), values come in layers (schema defaults, then the user file, then the profile
// file, then --set overrides), and files are TOML with a schema_version and unknown keys kept. The launcher
// renders its settings pages from the Schema; dq8-game reads a Store (--config, --set); dq8ctl settings uses
// the same API. Errors throw Error with settings_parse (not TOML) or settings_invalid (unknown key in the
// schema itself, wrong type, out of range), naming the file and the key. M5 day 0: declarations; M5 session A
// implements them.
namespace dq8::core::settings {

enum class Type { boolean, integer, real, choice, text, path };

// bool for boolean, int64 for integer, double for real, string for choice, text and path.
using Value = std::variant<bool, std::int64_t, double, std::string>;

struct Setting {
    std::string key; // "display.window_mode": category, a dot, a lower-case name
    Type type = Type::boolean;
    Value default_value;
    std::vector<std::string> choices; // Type::choice, in display order
    std::int64_t min = 0;             // Type::integer, inclusive
    std::int64_t max = 0;
    double min_real = 0.0; // Type::real, inclusive
    double max_real = 0.0;
    std::string label;             // a string-table key (config/settings/strings/<language>.toml)
    std::string description;       // a string-table key
    std::string category;          // the settings page: display, audio, input, game, advanced
    bool advanced = false;         // shown only when the page's advanced toggle is on
    bool requires_restart = false; // a running game picks the value up only when started again
    bool hot_reload = false;       // a running game applies the value at once (M5: none yet)
    std::vector<std::string>
        conditions;    // the schema's "requires": "key=value" pairs; shown only when all hold
    std::string owner; // the component that reads it ("host", "runtime.platform", ...)
};

class Schema {
  public:
    // Reads and checks a schema file: unique keys, a known type, a default inside the range or choices.
    static Schema load(const std::filesystem::path& file);
    static Schema parse(std::string_view toml_text, std::string_view origin);

    int version() const noexcept;
    const std::vector<Setting>& settings() const noexcept;
    const Setting* find(std::string_view key) const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

// Label and description texts by string-table key, for one language.
class StringTable {
  public:
    static StringTable load(const std::filesystem::path& file);
    // The text for `key`, or `key` itself when the table lacks it.
    std::string_view get(std::string_view key) const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

enum class Layer { defaults, user, profile, overrides }; // later layers win

class Store {
  public:
    explicit Store(Schema schema);
    ~Store();
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;

    const Schema& schema() const noexcept;

    // Reads a settings file into `layer`, replacing what the layer held. Keys the schema does not know are
    // kept (save writes them back) and listed in warnings(); a known key with a bad value throws
    // settings_invalid. A missing file leaves the layer empty.
    void load(Layer layer, const std::filesystem::path& file);
    void load_text(Layer layer, std::string_view toml_text, std::string_view origin);
    // Writes `layer` as TOML: schema_version, then the layer's values by category, then the unknown keys.
    void save(Layer layer, const std::filesystem::path& file) const;

    // The --set form, "key=value", into Layer::overrides; the text is parsed by the key's type.
    void set_from_text(std::string_view assignment);
    void set(Layer layer, std::string_view key, Value value);
    void reset(Layer layer, std::string_view key);

    // The effective value: the highest layer that holds the key, else the schema default. Throws
    // settings_invalid for a key the schema does not know.
    Value get(std::string_view key) const;
    bool get_bool(std::string_view key) const;
    std::int64_t get_int(std::string_view key) const;
    double get_real(std::string_view key) const;
    std::string get_string(std::string_view key) const; // choice, text and path
    Layer source(std::string_view key) const;

    const std::vector<std::string>& warnings() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The schema and string tables shipped with the program: config/settings/ in the working directory or an
// ancestor of `executable_dir`, else share/dq8/settings next to it. Throws Error(io_open) when none is found.
std::filesystem::path find_settings_dir(const std::filesystem::path& executable_dir);

// The user's settings file: settings.toml in platform::user_config_dir(), or next to the executable when a
// file named "portable" sits there.
std::filesystem::path default_user_file(const std::filesystem::path& executable_dir);

} // namespace dq8::core::settings
