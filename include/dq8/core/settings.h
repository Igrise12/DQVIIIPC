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
// (config/settings/schema.toml), values come in layers (schema defaults, then the user file, then the
// per-profile overrides, then --set), and files are TOML with a schema_version and unknown keys kept. The
// launcher renders its settings pages from the Schema; dq8-game reads a Store (--config, --set); dq8ctl
// settings uses the same API.
//
// Errors throw Error: settings_parse for a schema, string table or settings file that is not TOML (the
// message starts with "<file>:<line>:"), settings_invalid for a schema that breaks its own rules, for an
// unknown key given to get/set/reset, and for a value given to set/set_from_text that the setting does not
// allow, io_* for files that cannot be read or written. A settings *file* never throws for its values: an
// unknown key is kept and an invalid value is dropped (the default applies), and both are listed in
// warnings().
namespace dq8::core::settings {

enum class Type { boolean, integer, real, choice, text, path };

// "boolean", "integer", "real", "choice", "text", "path".
std::string_view to_string(Type type) noexcept;

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

    // The table a settings file keeps the key in, and the key's name inside it: "display", "window_mode".
    std::string_view file_table() const noexcept;
    std::string_view file_name() const noexcept;
};

// A value as the command line and the launcher's text fields write it: true/false, 1280, 1.5, windowed.
std::string to_text(const Value& value);
// Parses `text` by the setting's type and checks it (range, choices). Booleans also take on/off, yes/no and
// 1/0. Throws settings_invalid naming the key and what it allows.
Value parse_value(const Setting& setting, std::string_view text);
// Checks a value against the setting (the variant's type, the range, the choices; an integer is accepted for
// a real). Returns an empty string when it is allowed, else why not.
std::string check_value(const Setting& setting, const Value& value);
// What the setting allows, for messages: "true or false", "640 to 7680", "windowed, borderless or
// fullscreen".
std::string describe_allowed(const Setting& setting);

class Schema {
  public:
    // Reads and checks a schema file: unique keys, a known type, a default inside the range or choices,
    // conditions naming other settings and values they can take, no unknown fields.
    static Schema load(const std::filesystem::path& file);
    static Schema parse(std::string_view toml_text, std::string_view origin);

    int version() const noexcept;
    const std::vector<Setting>& settings() const noexcept;
    const Setting* find(std::string_view key) const noexcept;
    // The categories in the order their first setting appears.
    const std::vector<std::string>& categories() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

// Label and description texts by string-table key, for one language.
class StringTable {
  public:
    static StringTable load(const std::filesystem::path& file);
    static StringTable parse(std::string_view toml_text, std::string_view origin);
    // The text for `key`, or `key` itself when the table lacks it.
    std::string_view get(std::string_view key) const noexcept;
    bool contains(std::string_view key) const noexcept;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

enum class Layer { defaults, user, profile, overrides }; // later layers win

// "defaults", "user", "profile", "overrides".
std::string_view to_string(Layer layer) noexcept;

// The layered values. A settings file holds schema_version, one table per category ([display], [audio], ...)
// with the settings by name, and for the user file per-profile tables ([profile.<id>.<category>]).
// Layer::user is the file's category tables; Layer::profile is the [profile.<id>] tables of the user file for
// the profile chosen with use_profile() (empty until one is chosen); Layer::overrides is --set. Unknown keys,
// comments and the order of the file are kept on save.
class Store {
  public:
    explicit Store(Schema schema);
    ~Store();
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;

    const Schema& schema() const noexcept;

    // Reads a settings file into `layer`, replacing what the layer held (for Layer::user also every profile's
    // overrides; for Layer::profile the chosen profile's, from the file's category tables). Unknown keys are
    // kept and invalid values dropped, both listed in warnings() as "<file>:<line>: ...". A missing file
    // leaves the layer empty. Layer::defaults throws settings_invalid.
    void load(Layer layer, const std::filesystem::path& file);
    void load_text(Layer layer, std::string_view toml_text, std::string_view origin);
    // Writes `layer` as TOML, atomically (to_toml()).
    void save(Layer layer, const std::filesystem::path& file) const;
    // What save() writes: schema_version, then the layer's values by category (for Layer::user also the
    // profiles' tables), then the unknown keys; the order and comments of a loaded file are kept. For
    // Layer::defaults every setting with its default value.
    std::string to_toml(Layer layer) const;

    // Chooses the profile whose [profile.<id>] tables form Layer::profile (empty: none).
    void use_profile(std::string_view profile_id);
    const std::string& profile() const noexcept;

    // The --set form, "key=value", into Layer::overrides; the text is parsed by the key's type.
    void set_from_text(std::string_view assignment);
    // Throws settings_invalid for an unknown key, a value the setting does not allow, Layer::defaults, and
    // Layer::profile while no profile is chosen.
    void set(Layer layer, std::string_view key, Value value);
    void reset(Layer layer, std::string_view key);
    // Removes every value of `layer` (unknown keys stay).
    void clear(Layer layer);
    bool holds(Layer layer, std::string_view key) const;

    // The effective value: the highest layer that holds the key, else the schema default. Throws
    // settings_invalid for a key the schema does not know.
    Value get(std::string_view key) const;
    bool get_bool(std::string_view key) const;
    std::int64_t get_int(std::string_view key) const;
    double get_real(std::string_view key) const;
    std::string get_string(std::string_view key) const; // choice, text and path
    Layer source(std::string_view key) const;
    // True when every condition of the setting ("requires") holds for the effective values.
    bool applies(std::string_view key) const;

    const std::vector<std::string>& warnings() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The schema and string tables shipped with the program: config/settings/ under the working directory or
// under `executable_dir` or an ancestor (the source tree), else share/dq8/settings there (an install). Throws
// Error(io_open) when none is found.
std::filesystem::path find_settings_dir(const std::filesystem::path& executable_dir);

// The user's settings file: settings.toml in platform::user_config_dir(), or next to the executable when a
// file named "portable" sits there.
std::filesystem::path default_user_file(const std::filesystem::path& executable_dir);

} // namespace dq8::core::settings
