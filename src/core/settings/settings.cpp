// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The settings schema, string tables and the layered store (dq8/core/settings.h, ADR-0006). Files are read
// with toml11's ordered values, so a settings file written back keeps its order, its comments and the keys
// this version does not know.
#include "dq8/core/settings.h"

#include "dq8/core/error.h"
#include "platform/files.h"
#include "platform/paths.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <toml.hpp>
#include <utility>

namespace dq8::core::settings {

namespace fs = std::filesystem;

namespace {

using toml_value = toml::ordered_value;
using toml_table = toml::ordered_table;
using ValueMap = std::map<std::string, Value, std::less<>>;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

[[noreturn]] void invalid(const std::string& message) {
    throw Error(ErrorCode::settings_invalid, message);
}

std::size_t line_of(const toml_value& v) {
    return v.location().first_line_number();
}

std::string where(std::string_view origin, const toml_value& v) {
    return std::string(origin) + ":" + std::to_string(line_of(v)) + ": ";
}

// Parses TOML text; a syntax error is settings_parse with "<origin>:<line>: <title>" and toml11's excerpt.
toml_value parse_document(std::string_view text, std::string_view origin) {
    std::istringstream stream{std::string(text)};
    auto parsed = toml::try_parse<toml::ordered_type_config>(stream, std::string(origin));
    if (parsed.is_err()) {
        std::string message;
        for (const toml::error_info& e : parsed.unwrap_err()) {
            const std::size_t line =
                e.locations().empty() ? 0 : e.locations().front().first.first_line_number();
            message += (message.empty() ? "" : "\n") + std::string(origin) + ":" + std::to_string(line) +
                       ": " + e.title() + "\n" + toml::format_error(e);
        }
        throw Error(ErrorCode::settings_parse, message);
    }
    return std::move(parsed.unwrap());
}

// "category.name": lower case, digits and '_', one dot, neither side empty.
bool valid_key(std::string_view key) {
    const std::size_t dot = key.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size() ||
        key.find('.', dot + 1) != std::string_view::npos) {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

std::optional<Type> parse_type(std::string_view name) {
    for (const Type t : {Type::boolean, Type::integer, Type::real, Type::choice, Type::text, Type::path}) {
        if (name == to_string(t)) {
            return t;
        }
    }
    return std::nullopt;
}

std::string in_quotes(std::string_view s) {
    return "'" + std::string(s) + "'";
}

std::string format_real(double d) {
    char buf[64];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, d);
    if (ec != std::errc()) {
        return std::to_string(d);
    }
    std::string s(buf, end);
    // A real always reads back as one: "2" becomes "2.0".
    if (s.find_first_of(".eEn") == std::string::npos) {
        s += ".0";
    }
    return s;
}

// A TOML value of a file as a setting's value, or why it is not one.
std::pair<std::optional<Value>, std::string> from_toml(const Setting& s, const toml_value& v) {
    Value value;
    switch (s.type) {
    case Type::boolean:
        if (!v.is_boolean()) {
            return {std::nullopt, "is not true or false"};
        }
        value = v.as_boolean();
        break;
    case Type::integer:
        if (!v.is_integer()) {
            return {std::nullopt, "is not a whole number"};
        }
        value = static_cast<std::int64_t>(v.as_integer());
        break;
    case Type::real:
        if (v.is_integer()) {
            value = static_cast<double>(v.as_integer());
        } else if (v.is_floating()) {
            value = static_cast<double>(v.as_floating());
        } else {
            return {std::nullopt, "is not a number"};
        }
        break;
    case Type::choice:
    case Type::text:
    case Type::path:
        if (!v.is_string()) {
            return {std::nullopt, "is not a string"};
        }
        value = v.as_string();
        break;
    }
    std::string why = check_value(s, value);
    if (!why.empty()) {
        return {std::nullopt, std::move(why)};
    }
    return {std::move(value), {}};
}

// "why" with what the setting allows, unless it says so already ("is outside 10 to 100").
std::string with_allowed(const Setting& s, const std::string& why) {
    const std::string allowed = describe_allowed(s);
    return why.find(allowed) != std::string::npos ? why : why + " (allowed: " + allowed + ")";
}

toml_value to_toml_value(const Value& value) {
    return std::visit([](const auto& v) { return toml_value(v); }, value);
}

// Replaces a value inside a document and keeps the comments attached to it.
void assign_keep_comments(toml_value& slot, toml_value value) {
    auto comments = slot.comments();
    slot = std::move(value);
    slot.comments() = std::move(comments);
}

toml_value new_table() {
    return toml_value(toml_table{});
}

// A table whose children are all tables needs no header of its own ([profile] above [profile.ID.display]).
void hide_header_if_only_tables(toml_value& t) {
    if (!t.is_table() || t.as_table().empty()) {
        return;
    }
    const bool only_tables = std::all_of(t.as_table().begin(), t.as_table().end(),
                                         [](const auto& entry) { return entry.second.is_table(); });
    if (only_tables) {
        t.as_table_fmt().fmt = toml::table_format::implicit;
    }
}

} // namespace

std::string_view to_string(Type type) noexcept {
    switch (type) {
    case Type::boolean:
        return "boolean";
    case Type::integer:
        return "integer";
    case Type::real:
        return "real";
    case Type::choice:
        return "choice";
    case Type::text:
        return "text";
    case Type::path:
        return "path";
    }
    return "unknown";
}

std::string_view to_string(Layer layer) noexcept {
    switch (layer) {
    case Layer::defaults:
        return "defaults";
    case Layer::user:
        return "user";
    case Layer::profile:
        return "profile";
    case Layer::overrides:
        return "overrides";
    }
    return "unknown";
}

std::string_view Setting::file_table() const noexcept {
    return std::string_view(key).substr(0, key.find('.'));
}

std::string_view Setting::file_name() const noexcept {
    const std::size_t dot = key.find('.');
    return dot == std::string::npos ? std::string_view(key) : std::string_view(key).substr(dot + 1);
}

std::string to_text(const Value& value) {
    if (const bool* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
    }
    if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
    }
    if (const double* d = std::get_if<double>(&value)) {
        return format_real(*d);
    }
    return std::get<std::string>(value);
}

std::string describe_allowed(const Setting& s) {
    switch (s.type) {
    case Type::boolean:
        return "true or false";
    case Type::integer:
        return std::to_string(s.min) + " to " + std::to_string(s.max);
    case Type::real:
        return format_real(s.min_real) + " to " + format_real(s.max_real);
    case Type::choice: {
        std::string out;
        for (std::size_t i = 0; i < s.choices.size(); ++i) {
            if (i != 0) {
                out += i + 1 == s.choices.size() ? " or " : ", ";
            }
            out += s.choices[i];
        }
        return out;
    }
    case Type::text:
        return "any text";
    case Type::path:
        return "a path, or empty for the default";
    }
    return "?";
}

std::string check_value(const Setting& s, const Value& value) {
    switch (s.type) {
    case Type::boolean:
        return std::holds_alternative<bool>(value) ? std::string() : "is not true or false";
    case Type::integer: {
        const std::int64_t* i = std::get_if<std::int64_t>(&value);
        if (i == nullptr) {
            return "is not a whole number";
        }
        if (*i < s.min || *i > s.max) {
            return "is outside " + describe_allowed(s);
        }
        return {};
    }
    case Type::real: {
        double d = 0.0;
        if (const double* r = std::get_if<double>(&value)) {
            d = *r;
        } else if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) {
            d = static_cast<double>(*i);
        } else {
            return "is not a number";
        }
        if (!std::isfinite(d) || d < s.min_real || d > s.max_real) {
            return "is outside " + describe_allowed(s);
        }
        return {};
    }
    case Type::choice: {
        const std::string* c = std::get_if<std::string>(&value);
        if (c == nullptr) {
            return "is not one of " + describe_allowed(s);
        }
        if (std::find(s.choices.begin(), s.choices.end(), *c) == s.choices.end()) {
            return "is not one of " + describe_allowed(s);
        }
        return {};
    }
    case Type::text:
    case Type::path:
        return std::holds_alternative<std::string>(value) ? std::string() : "is not text";
    }
    return "has an unknown type";
}

Value parse_value(const Setting& s, std::string_view text) {
    const auto fail = [&](const std::string& why) -> Value {
        invalid(s.key + " = '" + std::string(text) + "' " + with_allowed(s, why));
    };
    Value value;
    switch (s.type) {
    case Type::boolean:
        if (text == "true" || text == "on" || text == "yes" || text == "1") {
            value = true;
        } else if (text == "false" || text == "off" || text == "no" || text == "0") {
            value = false;
        } else {
            return fail("is not true or false");
        }
        break;
    case Type::integer: {
        std::int64_t i = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), i, 10);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            return fail("is not a whole number");
        }
        value = i;
        break;
    }
    case Type::real: {
        double d = 0.0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), d);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            return fail("is not a number");
        }
        value = d;
        break;
    }
    case Type::choice:
    case Type::text:
    case Type::path:
        value = std::string(text);
        break;
    }
    if (const std::string why = check_value(s, value); !why.empty()) {
        return fail(why);
    }
    return value;
}

// --- Schema ---

struct Schema::Impl {
    int version = 0;
    std::vector<Setting> settings;
    std::vector<std::string> categories;
    std::map<std::string, std::size_t, std::less<>> index;
};

Schema Schema::load(const fs::path& file) {
    return parse(platform::read_text_file(file, 4u << 20), utf8(file));
}

Schema Schema::parse(std::string_view toml_text, std::string_view origin) {
    const toml_value root = parse_document(toml_text, origin);
    auto impl = std::make_shared<Impl>();
    const auto fail = [&](const toml_value& at, const std::string& message) {
        invalid(where(origin, at) + message);
    };

    for (const auto& [key, v] : root.as_table()) {
        if (key != "schema_version" && key != "setting") {
            fail(v, "unknown key " + in_quotes(key) + " (a schema has schema_version and [[setting]] rows)");
        }
    }
    if (!root.contains("schema_version") || !root.at("schema_version").is_integer() ||
        root.at("schema_version").as_integer() < 1 || root.at("schema_version").as_integer() > 1'000'000) {
        fail(root.contains("schema_version") ? root.at("schema_version") : root,
             "schema_version must be a positive integer");
    }
    impl->version = static_cast<int>(root.at("schema_version").as_integer());
    if (!root.contains("setting")) {
        fail(root, "the schema has no [[setting]] rows");
    }
    const toml_value& rows = root.at("setting");
    if (!rows.is_array()) {
        fail(rows, "setting must be an array of tables ([[setting]])");
    }

    const std::set<std::string, std::less<>> fields{
        "key",         "type",     "default",  "choices",          "min",        "max",      "label",
        "description", "category", "advanced", "requires_restart", "hot_reload", "requires", "owner"};
    for (const toml_value& row : rows.as_array()) {
        if (!row.is_table()) {
            fail(row, "every setting must be a table ([[setting]])");
        }
        for (const auto& [field, v] : row.as_table()) {
            if (fields.count(field) == 0) {
                fail(v, "unknown field " + in_quotes(field));
            }
        }
        const auto string_field = [&](const char* name, bool required) -> std::string {
            if (!row.contains(name)) {
                if (required) {
                    fail(row, std::string("a setting needs '") + name + "'");
                }
                return {};
            }
            const toml_value& v = row.at(name);
            if (!v.is_string() || (required && v.as_string().empty())) {
                fail(v, std::string("'") + name + "' must be a non-empty string");
            }
            return v.as_string();
        };
        const auto bool_field = [&](const char* name) {
            if (!row.contains(name)) {
                return false;
            }
            const toml_value& v = row.at(name);
            if (!v.is_boolean()) {
                fail(v, std::string("'") + name + "' must be true or false");
            }
            return v.as_boolean();
        };

        Setting s;
        s.key = string_field("key", true);
        if (!valid_key(s.key)) {
            fail(row.at("key"),
                 in_quotes(s.key) + " is not a key: category.name, lower case, digits and '_'");
        }
        if (impl->index.count(s.key) != 0) {
            fail(row.at("key"), in_quotes(s.key) + " is declared twice");
        }
        const std::string type_name = string_field("type", true);
        const std::optional<Type> type = parse_type(type_name);
        if (!type) {
            fail(row.at("type"), s.key + ": unknown type " + in_quotes(type_name) +
                                     " (boolean, integer, real, choice, text, path)");
        }
        s.type = *type;
        s.label = string_field("label", true);
        s.description = string_field("description", true);
        s.category = string_field("category", true);
        s.owner = string_field("owner", true);
        s.advanced = bool_field("advanced");
        s.requires_restart = bool_field("requires_restart");
        s.hot_reload = bool_field("hot_reload");

        const bool ranged = s.type == Type::integer || s.type == Type::real;
        if (row.contains("choices") != (s.type == Type::choice)) {
            fail(row, s.key + ": 'choices' belongs to (and is needed by) a choice setting");
        }
        if ((row.contains("min") || row.contains("max")) && !ranged) {
            fail(row, s.key + ": 'min' and 'max' belong to integer and real settings");
        }
        if (ranged && (!row.contains("min") || !row.contains("max"))) {
            fail(row, s.key + ": an " + type_name + " setting needs 'min' and 'max'");
        }
        if (s.type == Type::choice) {
            const toml_value& c = row.at("choices");
            if (!c.is_array() || c.as_array().empty()) {
                fail(c, s.key + ": 'choices' must be a non-empty array of strings");
            }
            for (const toml_value& item : c.as_array()) {
                if (!item.is_string() || item.as_string().empty()) {
                    fail(item, s.key + ": every choice must be a non-empty string");
                }
                if (std::find(s.choices.begin(), s.choices.end(), item.as_string()) != s.choices.end()) {
                    fail(item, s.key + ": choice " + in_quotes(item.as_string()) + " is listed twice");
                }
                s.choices.push_back(item.as_string());
            }
        }
        if (s.type == Type::integer) {
            const toml_value& lo = row.at("min");
            const toml_value& hi = row.at("max");
            if (!lo.is_integer() || !hi.is_integer() || lo.as_integer() > hi.as_integer()) {
                fail(lo, s.key + ": 'min' and 'max' must be whole numbers with min <= max");
            }
            s.min = lo.as_integer();
            s.max = hi.as_integer();
        }
        if (s.type == Type::real) {
            const auto number = [&](const toml_value& v) {
                if (!v.is_integer() && !v.is_floating()) {
                    fail(v, s.key + ": 'min' and 'max' must be numbers");
                }
                return v.is_integer() ? static_cast<double>(v.as_integer())
                                      : static_cast<double>(v.as_floating());
            };
            s.min_real = number(row.at("min"));
            s.max_real = number(row.at("max"));
            if (!(s.min_real <= s.max_real)) {
                fail(row.at("min"), s.key + ": 'min' must not exceed 'max'");
            }
        }
        if (!row.contains("default")) {
            fail(row, s.key + ": a setting needs 'default'");
        }
        auto [def, why] = from_toml(s, row.at("default"));
        if (!def) {
            fail(row.at("default"), s.key + ": the default " + why);
        }
        s.default_value = std::move(*def);

        if (row.contains("requires")) {
            const toml_value& r = row.at("requires");
            if (!r.is_array()) {
                fail(r, s.key + ": 'requires' must be an array of \"key=value\" strings");
            }
            for (const toml_value& item : r.as_array()) {
                if (!item.is_string() || item.as_string().find('=') == std::string::npos) {
                    fail(item, s.key + ": every 'requires' entry must be \"key=value\"");
                }
                s.conditions.push_back(item.as_string());
            }
        }

        if (std::find(impl->categories.begin(), impl->categories.end(), s.category) ==
            impl->categories.end()) {
            impl->categories.push_back(s.category);
        }
        impl->index.emplace(s.key, impl->settings.size());
        impl->settings.push_back(std::move(s));
    }

    // Conditions name another setting and a value it can take.
    for (std::size_t i = 0; i < impl->settings.size(); ++i) {
        const Setting& s = impl->settings[i];
        for (const std::string& condition : s.conditions) {
            const std::size_t eq = condition.find('=');
            const std::string other = condition.substr(0, eq);
            const auto it = impl->index.find(other);
            const toml_value& at = rows.as_array()[i];
            if (it == impl->index.end() || other == s.key) {
                fail(at, s.key + ": 'requires' names " + in_quotes(other) + ", which is not another setting");
            }
            try {
                (void)parse_value(impl->settings[it->second], condition.substr(eq + 1));
            } catch (const Error& e) {
                fail(at, s.key + ": 'requires' " + in_quotes(condition) + ": " + e.what());
            }
        }
    }

    Schema schema;
    schema.impl_ = std::move(impl);
    return schema;
}

int Schema::version() const noexcept {
    return impl_ ? impl_->version : 0;
}

const std::vector<Setting>& Schema::settings() const noexcept {
    static const std::vector<Setting> none;
    return impl_ ? impl_->settings : none;
}

const Setting* Schema::find(std::string_view key) const noexcept {
    if (!impl_) {
        return nullptr;
    }
    const auto it = impl_->index.find(key);
    return it == impl_->index.end() ? nullptr : &impl_->settings[it->second];
}

const std::vector<std::string>& Schema::categories() const noexcept {
    static const std::vector<std::string> none;
    return impl_ ? impl_->categories : none;
}

// --- StringTable ---

struct StringTable::Impl {
    std::map<std::string, std::string, std::less<>> strings;
};

StringTable StringTable::load(const fs::path& file) {
    return parse(platform::read_text_file(file, 4u << 20), utf8(file));
}

StringTable StringTable::parse(std::string_view toml_text, std::string_view origin) {
    const toml_value root = parse_document(toml_text, origin);
    for (const auto& [key, v] : root.as_table()) {
        if (key != "strings") {
            invalid(where(origin, v) + "unknown key " + in_quotes(key) +
                    " (a string table has one [strings])");
        }
    }
    if (!root.contains("strings") || !root.at("strings").is_table()) {
        invalid(where(origin, root) + "a string table needs a [strings] table");
    }
    auto impl = std::make_shared<Impl>();
    for (const auto& [key, v] : root.at("strings").as_table()) {
        if (!v.is_string()) {
            invalid(where(origin, v) + in_quotes(key) + " must be a string");
        }
        impl->strings.emplace(key, v.as_string());
    }
    StringTable table;
    table.impl_ = std::move(impl);
    return table;
}

std::string_view StringTable::get(std::string_view key) const noexcept {
    if (impl_) {
        if (const auto it = impl_->strings.find(key); it != impl_->strings.end()) {
            return it->second;
        }
    }
    return key;
}

bool StringTable::contains(std::string_view key) const noexcept {
    return impl_ && impl_->strings.find(key) != impl_->strings.end();
}

// --- Store ---

struct Store::Impl {
    explicit Impl(Schema s) : schema(std::move(s)) {
        for (const Setting& setting : schema.settings()) {
            tables.insert(std::string(setting.file_table()));
        }
    }

    // A file as read: its document (order, comments, unknown keys) and where it came from.
    struct Document {
        std::optional<toml_value> doc;
        std::string origin;
    };

    Schema schema;
    std::set<std::string, std::less<>> tables; // the category tables of a settings file
    ValueMap user;
    ValueMap overrides;
    std::map<std::string, ValueMap, std::less<>> profiles; // [profile.<id>] of the user file
    std::string profile;
    Document user_doc;
    Document profile_doc;
    Document overrides_doc;
    std::vector<std::string> warnings;

    const Setting& setting(std::string_view key) const {
        const Setting* s = schema.find(key);
        if (s == nullptr) {
            invalid("unknown setting " + in_quotes(key));
        }
        return *s;
    }

    const ValueMap* values(Layer layer) const {
        switch (layer) {
        case Layer::defaults:
            return nullptr;
        case Layer::user:
            return &user;
        case Layer::profile: {
            const auto it = profiles.find(profile);
            return profile.empty() || it == profiles.end() ? nullptr : &it->second;
        }
        case Layer::overrides:
            return &overrides;
        }
        return nullptr;
    }

    ValueMap& writable(Layer layer) {
        switch (layer) {
        case Layer::defaults:
            break;
        case Layer::user:
            return user;
        case Layer::profile:
            if (profile.empty()) {
                invalid("the profile layer needs a profile (use_profile)");
            }
            return profiles[profile];
        case Layer::overrides:
            return overrides;
        }
        invalid("the defaults come from the schema and cannot be changed");
    }

    Document& document(Layer layer) {
        switch (layer) {
        case Layer::user:
            return user_doc;
        case Layer::profile:
            return profile_doc;
        default:
            return overrides_doc;
        }
    }

    // The settings of one category table into `out`; unknown and invalid entries become warnings.
    void read_category(const std::string& table, const toml_value& t, std::string_view origin,
                       const std::string& prefix, ValueMap& out) {
        for (const auto& [name, v] : t.as_table()) {
            const std::string key = table + "." + name;
            const Setting* s = schema.find(key);
            if (s == nullptr) {
                warnings.push_back(where(origin, v) + "unknown setting " + in_quotes(prefix + key) +
                                   " (kept as it is)");
                continue;
            }
            auto [value, why] = from_toml(*s, v);
            if (!value) {
                warnings.push_back(where(origin, v) + prefix + key + " = " + toml::format(v) + " " +
                                   with_allowed(*s, why) + "; ignored, the default applies");
                continue;
            }
            out[key] = std::move(*value);
        }
    }

    // A settings file's category tables into `out`, and with `profile_out` its [profile.<id>] tables.
    void read_document(const toml_value& root, std::string_view origin, ValueMap& out,
                       std::map<std::string, ValueMap, std::less<>>* profile_out) {
        for (const auto& [key, v] : root.as_table()) {
            if (key == "schema_version") {
                if (!v.is_integer() || v.as_integer() < 1) {
                    warnings.push_back(where(origin, v) + "schema_version must be a positive integer");
                } else if (v.as_integer() > schema.version()) {
                    warnings.push_back(where(origin, v) + "written for schema_version " +
                                       std::to_string(v.as_integer()) + ", newer than this program's " +
                                       std::to_string(schema.version()) + "; keys it does not know are kept");
                }
            } else if (key == "profile" && profile_out != nullptr && v.is_table()) {
                for (const auto& [id, per_profile] : v.as_table()) {
                    if (!per_profile.is_table()) {
                        warnings.push_back(where(origin, per_profile) + "unknown key " +
                                           in_quotes("profile." + id) + " (kept as it is)");
                        continue;
                    }
                    ValueMap& values = (*profile_out)[id];
                    for (const auto& [table, t] : per_profile.as_table()) {
                        if (tables.count(table) == 0 || !t.is_table()) {
                            warnings.push_back(where(origin, t) + "unknown key " +
                                               in_quotes("profile." + id + "." + table) + " (kept as it is)");
                            continue;
                        }
                        read_category(table, t, origin, "profile." + id + ".", values);
                    }
                }
            } else if (tables.count(key) != 0 && v.is_table()) {
                read_category(key, v, origin, "", out);
            } else {
                warnings.push_back(where(origin, v) + "unknown key " + in_quotes(key) + " (kept as it is)");
            }
        }
    }

    // Brings the category tables under `root` in line with `values`: known keys are replaced in place or
    // removed, new ones appended in schema order, emptied category tables removed; anything else is kept.
    void write_values(toml_value& root, const ValueMap& values) const {
        std::set<std::string, std::less<>> written;
        auto& top = root.as_table();
        for (auto& [table, t] : top) {
            if (tables.count(table) == 0 || !t.is_table()) {
                continue;
            }
            auto& entries = t.as_table();
            for (auto it = entries.begin(); it != entries.end();) {
                const std::string key = table + "." + it->first;
                if (schema.find(key) == nullptr) {
                    ++it;
                    continue;
                }
                if (const auto v = values.find(key); v != values.end()) {
                    assign_keep_comments(it->second, to_toml_value(v->second));
                    written.insert(key);
                    ++it;
                } else {
                    it = entries.erase(it);
                }
            }
        }
        for (const Setting& s : schema.settings()) {
            const auto v = values.find(s.key);
            if (v == values.end() || written.count(s.key) != 0) {
                continue;
            }
            const std::string table(s.file_table());
            if (!top.contains(table) || !top.at(table).is_table()) {
                top[table] = new_table();
            }
            top.at(table).as_table()[std::string(s.file_name())] = to_toml_value(v->second);
        }
        for (auto it = top.begin(); it != top.end();) {
            if (tables.count(it->first) != 0 && it->second.is_table() && it->second.as_table().empty() &&
                it->second.comments().empty()) {
                it = top.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::string render(Layer layer) const {
        const Document* source = nullptr;
        switch (layer) {
        case Layer::defaults:
            break;
        case Layer::user:
            source = &user_doc;
            break;
        case Layer::profile:
            source = &profile_doc;
            break;
        case Layer::overrides:
            source = &overrides_doc;
            break;
        }
        const bool fresh = source == nullptr || !source->doc;
        toml_value root = fresh ? new_table() : *source->doc;
        if (root.contains("schema_version")) {
            assign_keep_comments(root.as_table().at("schema_version"),
                                 toml_value(static_cast<std::int64_t>(schema.version())));
        } else {
            // First in a new file; after the existing keys of a loaded one.
            toml_table reordered;
            reordered.emplace("schema_version", toml_value(static_cast<std::int64_t>(schema.version())));
            for (auto& entry : root.as_table()) {
                reordered.emplace(entry.first, entry.second);
            }
            auto comments = root.comments();
            root = toml_value(std::move(reordered));
            root.comments() = std::move(comments);
        }

        if (layer == Layer::defaults) {
            ValueMap all;
            for (const Setting& s : schema.settings()) {
                all[s.key] = s.default_value;
            }
            write_values(root, all);
        } else {
            static const ValueMap empty;
            const ValueMap* v = values(layer);
            write_values(root, v != nullptr ? *v : empty);
        }

        if (layer == Layer::user) {
            auto& top = root.as_table();
            for (const auto& [id, values] : profiles) {
                if (values.empty() && !(top.contains("profile") && top.at("profile").is_table() &&
                                        top.at("profile").as_table().contains(id))) {
                    continue;
                }
                if (!top.contains("profile") || !top.at("profile").is_table()) {
                    top["profile"] = new_table();
                }
                auto& per = top.at("profile").as_table();
                if (!per.contains(id) || !per.at(id).is_table()) {
                    per[id] = new_table();
                }
                write_values(per.at(id), values);
            }
            if (top.contains("profile") && top.at("profile").is_table()) {
                auto& per = top.at("profile").as_table();
                for (auto it = per.begin(); it != per.end();) {
                    if (it->second.is_table() && it->second.as_table().empty() &&
                        it->second.comments().empty()) {
                        it = per.erase(it);
                    } else {
                        hide_header_if_only_tables(it->second);
                        ++it;
                    }
                }
                if (per.empty()) {
                    top.erase("profile");
                } else {
                    hide_header_if_only_tables(top.at("profile"));
                }
            }
        }

        std::string text = toml::format(root);
        if (fresh) {
            text = "# DQ8-Native settings (config/settings/schema.toml lists every key). Written by the "
                   "launcher "
                   "and\n# `dq8ctl settings`; keys this version does not know are kept.\n" +
                   text;
        }
        return text;
    }
};

Store::Store(Schema schema) : impl_(std::make_unique<Impl>(std::move(schema))) {}
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

const Schema& Store::schema() const noexcept {
    return impl_->schema;
}

void Store::load(Layer layer, const fs::path& file) {
    std::error_code ec;
    if (layer != Layer::defaults && !fs::exists(file, ec)) {
        load_text(layer, "", utf8(file));
        impl_->document(layer).doc.reset(); // nothing was read: save() writes a new file
        return;
    }
    load_text(layer, platform::read_text_file(file, 4u << 20), utf8(file));
}

void Store::load_text(Layer layer, std::string_view toml_text, std::string_view origin) {
    if (layer == Layer::defaults) {
        invalid("the defaults come from the schema; a file cannot be loaded into them");
    }
    if (layer == Layer::profile && impl_->profile.empty()) {
        invalid("the profile layer needs a profile (use_profile)");
    }
    toml_value root = parse_document(toml_text, origin);
    ValueMap values;
    std::map<std::string, ValueMap, std::less<>> profiles;
    impl_->read_document(root, origin, values, layer == Layer::user ? &profiles : nullptr);
    switch (layer) {
    case Layer::defaults:
        break;
    case Layer::user:
        impl_->user = std::move(values);
        impl_->profiles = std::move(profiles);
        break;
    case Layer::profile:
        impl_->profiles[impl_->profile] = std::move(values);
        break;
    case Layer::overrides:
        impl_->overrides = std::move(values);
        break;
    }
    Impl::Document& d = impl_->document(layer);
    d.doc = std::move(root);
    d.origin = std::string(origin);
}

void Store::save(Layer layer, const fs::path& file) const {
    platform::write_file_atomic(file, to_toml(layer));
}

std::string Store::to_toml(Layer layer) const {
    return impl_->render(layer);
}

void Store::use_profile(std::string_view profile_id) {
    impl_->profile = std::string(profile_id);
}

const std::string& Store::profile() const noexcept {
    return impl_->profile;
}

void Store::set_from_text(std::string_view assignment) {
    const std::size_t eq = assignment.find('=');
    if (eq == std::string_view::npos || eq == 0) {
        invalid("a setting is given as key=value, not " + in_quotes(assignment));
    }
    const Setting& s = impl_->setting(assignment.substr(0, eq));
    impl_->overrides[s.key] = parse_value(s, assignment.substr(eq + 1));
}

void Store::set(Layer layer, std::string_view key, Value value) {
    const Setting& s = impl_->setting(key);
    if (s.type == Type::real) {
        if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) {
            value = static_cast<double>(*i);
        }
    }
    if (const std::string why = check_value(s, value); !why.empty()) {
        invalid(s.key + " = " + to_text(value) + " " + with_allowed(s, why));
    }
    impl_->writable(layer)[s.key] = std::move(value);
}

void Store::reset(Layer layer, std::string_view key) {
    const Setting& s = impl_->setting(key);
    impl_->writable(layer).erase(s.key);
}

void Store::clear(Layer layer) {
    impl_->writable(layer).clear();
}

bool Store::holds(Layer layer, std::string_view key) const {
    const Setting& s = impl_->setting(key);
    const ValueMap* v = impl_->values(layer);
    return v != nullptr && v->count(s.key) != 0;
}

Value Store::get(std::string_view key) const {
    const Setting& s = impl_->setting(key);
    for (const Layer layer : {Layer::overrides, Layer::profile, Layer::user}) {
        if (const ValueMap* v = impl_->values(layer)) {
            if (const auto it = v->find(s.key); it != v->end()) {
                return it->second;
            }
        }
    }
    return s.default_value;
}

bool Store::get_bool(std::string_view key) const {
    const Value v = get(key);
    if (const bool* b = std::get_if<bool>(&v)) {
        return *b;
    }
    invalid(std::string(key) + " is not a boolean setting");
}

std::int64_t Store::get_int(std::string_view key) const {
    const Value v = get(key);
    if (const std::int64_t* i = std::get_if<std::int64_t>(&v)) {
        return *i;
    }
    invalid(std::string(key) + " is not an integer setting");
}

double Store::get_real(std::string_view key) const {
    const Value v = get(key);
    if (const double* d = std::get_if<double>(&v)) {
        return *d;
    }
    if (const std::int64_t* i = std::get_if<std::int64_t>(&v)) {
        return static_cast<double>(*i);
    }
    invalid(std::string(key) + " is not a real setting");
}

std::string Store::get_string(std::string_view key) const {
    Value v = get(key);
    if (std::string* s = std::get_if<std::string>(&v)) {
        return std::move(*s);
    }
    invalid(std::string(key) + " is not a text, path or choice setting");
}

Layer Store::source(std::string_view key) const {
    const Setting& s = impl_->setting(key);
    for (const Layer layer : {Layer::overrides, Layer::profile, Layer::user}) {
        if (const ValueMap* v = impl_->values(layer); v != nullptr && v->count(s.key) != 0) {
            return layer;
        }
    }
    return Layer::defaults;
}

bool Store::applies(std::string_view key) const {
    const Setting& s = impl_->setting(key);
    for (const std::string& condition : s.conditions) {
        const std::size_t eq = condition.find('=');
        if (to_text(get(std::string_view(condition).substr(0, eq))) != condition.substr(eq + 1)) {
            return false;
        }
    }
    return true;
}

const std::vector<std::string>& Store::warnings() const noexcept {
    return impl_->warnings;
}

fs::path find_settings_dir(const fs::path& executable_dir) {
    const fs::path found = platform::find_data_dir(
        executable_dir, {fs::path("config") / "settings", fs::path("share") / "dq8" / "settings"},
        "schema.toml");
    if (found.empty()) {
        throw Error(ErrorCode::io_open,
                    "no settings schema found (config/settings/schema.toml in the source tree, "
                    "share/dq8/settings in an install) near '" +
                        utf8(executable_dir) + "'");
    }
    return found;
}

fs::path default_user_file(const fs::path& executable_dir) {
    std::error_code ec;
    if (!executable_dir.empty() && fs::is_regular_file(executable_dir / "portable", ec)) {
        return executable_dir / "settings.toml";
    }
    return platform::user_config_dir() / "settings.toml";
}

} // namespace dq8::core::settings
