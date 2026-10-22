// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/settings_model.h"

#include "dq8/core/error.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace dq8::launcher {

namespace {

using core::settings::Layer;
using core::settings::Setting;
using core::settings::Type;
using core::settings::Value;

class StoreSettings final : public SettingsBackend {
  public:
    StoreSettings(core::settings::Store store, core::settings::StringTable strings,
                  std::filesystem::path file)
        : store_(std::move(store)), strings_(std::move(strings)), file_(std::move(file)) {}

    const std::vector<Setting>& settings() const override { return store_.schema().settings(); }
    std::string text(std::string_view string_key) const override {
        return std::string(strings_.get(string_key));
    }
    Value value(std::string_view key) const override { return store_.get(key); }
    bool user_set(std::string_view key) const override { return store_.source(key) == Layer::user; }
    void set(std::string_view key, const Value& value) override { store_.set(Layer::user, key, value); }
    void reset(std::string_view key) override { store_.reset(Layer::user, key); }
    void save() override {
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec); // save() names the file if this failed
        store_.save(Layer::user, file_);
    }
    std::filesystem::path file() const override { return file_; }
    std::vector<std::string> warnings() const override { return store_.warnings(); }

  private:
    core::settings::Store store_;
    core::settings::StringTable strings_;
    std::filesystem::path file_;
};

std::string format_real(double v) {
    char buf[64];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc() ? std::string(buf, end) : std::to_string(v);
}

} // namespace

MemorySettings::MemorySettings(std::vector<Setting> settings,
                               std::map<std::string, std::string, std::less<>> strings,
                               std::filesystem::path file)
    : settings_(std::move(settings)), strings_(std::move(strings)), file_(std::move(file)) {}

const Setting& MemorySettings::find(std::string_view key) const {
    for (const Setting& s : settings_) {
        if (s.key == key) {
            return s;
        }
    }
    throw core::Error(core::ErrorCode::settings_invalid, "unknown setting " + std::string(key));
}

std::string MemorySettings::text(std::string_view string_key) const {
    const auto it = strings_.find(string_key);
    return it != strings_.end() ? it->second : std::string(string_key);
}

Value MemorySettings::value(std::string_view key) const {
    const Setting& s = find(key);
    const auto it = user_.find(key);
    return it != user_.end() ? it->second : s.default_value;
}

void MemorySettings::set(std::string_view key, const Value& value) {
    const Setting& s = find(key);
    if (!value_matches_type(value, s.type)) {
        throw core::Error(core::ErrorCode::settings_invalid, s.key + ": a value of the wrong type");
    }
    user_[s.key] = value;
}

void MemorySettings::reset(std::string_view key) {
    const auto it = user_.find(key);
    if (it != user_.end()) {
        user_.erase(it);
    }
}

void MemorySettings::save() {
    if (fail_saves_) {
        throw core::Error(core::ErrorCode::io_write, "cannot write " + file_.string());
    }
    ++saves_;
}

std::unique_ptr<SettingsBackend> open_store_settings(const std::filesystem::path& executable_dir,
                                                     std::string_view language) {
    const std::filesystem::path dir = core::settings::find_settings_dir(executable_dir);
    core::settings::Schema schema = core::settings::Schema::load(dir / "schema.toml");
    core::settings::StringTable strings =
        core::settings::StringTable::load(dir / "strings" / (std::string(language) + ".toml"));
    core::settings::Store store(std::move(schema));
    const std::filesystem::path file = core::settings::default_user_file(executable_dir);
    store.load(Layer::user, file);
    return std::make_unique<StoreSettings>(std::move(store), std::move(strings), file);
}

std::string format_value(const Value& value) {
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

bool value_matches_type(const Value& value, Type type) noexcept {
    switch (type) {
    case Type::boolean:
        return std::holds_alternative<bool>(value);
    case Type::integer:
        return std::holds_alternative<std::int64_t>(value);
    case Type::real:
        return std::holds_alternative<double>(value);
    case Type::choice:
    case Type::text:
    case Type::path:
        return std::holds_alternative<std::string>(value);
    }
    return false;
}

SettingsModel::SettingsModel(std::unique_ptr<SettingsBackend> backend) : backend_(std::move(backend)) {
    for (const Setting& s : backend_->settings()) {
        if (std::find(categories_.begin(), categories_.end(), s.category) == categories_.end()) {
            categories_.push_back(s.category);
        }
    }
}

const Setting* SettingsModel::find(std::string_view key) const noexcept {
    for (const Setting& s : backend_->settings()) {
        if (s.key == key) {
            return &s;
        }
    }
    return nullptr;
}

std::string SettingsModel::category_title(std::string_view category) const {
    const std::string key = "category." + std::string(category);
    std::string title = backend_->text(key);
    if (title == key) { // no string: the category's name, capitalised
        title = std::string(category);
        if (!title.empty() && title[0] >= 'a' && title[0] <= 'z') {
            title[0] = static_cast<char>(title[0] - 'a' + 'A');
        }
    }
    return title;
}

bool SettingsModel::conditions_hold(const Setting& setting) const {
    for (const std::string& condition : setting.conditions) {
        const std::size_t eq = condition.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        const std::string_view key = std::string_view(condition).substr(0, eq);
        const std::string_view expected = std::string_view(condition).substr(eq + 1);
        if (find(key) == nullptr) {
            return false;
        }
        try {
            if (format_value(backend_->value(key)) != expected) {
                return false;
            }
        } catch (...) {
            return false;
        }
    }
    return true;
}

SettingRow SettingsModel::row(const Setting& s, bool game_running) const {
    SettingRow r;
    r.key = s.key;
    r.label = backend_->text(s.label.empty() ? s.key + ".label" : s.label);
    const std::string description_key = s.description.empty() ? s.key + ".description" : s.description;
    r.description = backend_->text(description_key);
    if (r.description == description_key) {
        r.description.clear(); // a missing description is left out; a missing label shows its key
    }
    r.default_value = s.default_value;
    try {
        r.value = backend_->value(s.key);
    } catch (...) {
        r.value = s.default_value;
    }
    if (!value_matches_type(r.value, s.type)) {
        r.value = s.default_value;
    }
    r.min = s.min;
    r.max = s.max;
    r.min_real = s.min_real;
    r.max_real = s.max_real;
    switch (s.type) {
    case Type::boolean:
        r.widget = Widget::checkbox;
        break;
    case Type::integer:
        r.widget = s.max > s.min && s.max - s.min <= 100 ? Widget::slider_int : Widget::input_int;
        break;
    case Type::real:
        r.widget = Widget::slider_real;
        break;
    case Type::choice:
        r.widget = Widget::combo;
        for (const std::string& c : s.choices) {
            const std::string text_key = s.key + ".choices." + c;
            std::string text = backend_->text(text_key);
            r.choices.push_back({c, text == text_key ? c : std::move(text)});
        }
        break;
    case Type::text:
        r.widget = Widget::text;
        break;
    case Type::path:
        r.widget = Widget::path;
        break;
    }
    r.advanced = s.advanced;
    r.requires_restart = s.requires_restart;
    r.modified = format_value(r.value) != format_value(r.default_value);
    if (game_running && !s.hot_reload) {
        r.hint = "The running game keeps the old value; it applies when the game starts again.";
    } else if (s.requires_restart) {
        r.hint = "Takes effect the next time the game starts.";
    }
    return r;
}

SettingsPage SettingsModel::page(std::string_view category, bool show_advanced, bool game_running) const {
    SettingsPage page;
    page.category = std::string(category);
    page.title = category_title(category);
    for (const Setting& s : backend_->settings()) {
        if (s.category != category) {
            continue;
        }
        if (!conditions_hold(s)) {
            ++page.conditions_hidden;
            continue;
        }
        if (s.advanced && !show_advanced) {
            ++page.advanced_hidden;
            continue;
        }
        page.rows.push_back(row(s, game_running));
    }
    return page;
}

bool SettingsModel::set(std::string_view key, const Value& value) {
    const Setting* s = find(key);
    if (s == nullptr) {
        problem_ = explain(core::ErrorCode::settings_invalid, "unknown setting " + std::string(key));
        return false;
    }
    if (!value_matches_type(value, s->type)) {
        problem_ = explain(core::ErrorCode::settings_invalid, s->key + ": a value of the wrong type");
        return false;
    }
    Value v = value;
    if (auto* i = std::get_if<std::int64_t>(&v); i != nullptr && s->max >= s->min) {
        *i = std::clamp(*i, s->min, s->max);
    }
    if (auto* d = std::get_if<double>(&v); d != nullptr && s->max_real >= s->min_real) {
        if (!std::isfinite(*d)) {
            problem_ = explain(core::ErrorCode::settings_invalid, s->key + ": not a number");
            return false;
        }
        *d = std::clamp(*d, s->min_real, s->max_real);
    }
    if (s->type == Type::choice &&
        std::find(s->choices.begin(), s->choices.end(), std::get<std::string>(v)) == s->choices.end()) {
        problem_ = explain(core::ErrorCode::settings_invalid,
                           s->key + ": '" + std::get<std::string>(v) + "' is not one of its choices");
        return false;
    }
    try {
        if (format_value(backend_->value(key)) == format_value(v)) {
            return true; // unchanged
        }
        backend_->set(key, v);
    } catch (...) {
        problem_ = explain_current_exception();
        return false;
    }
    dirty_ = true;
    return true;
}

void SettingsModel::reset(std::string_view key) {
    try {
        backend_->reset(key);
        dirty_ = true;
    } catch (...) {
        problem_ = explain_current_exception();
    }
}

void SettingsModel::reset_page(std::string_view category) {
    for (const Setting& s : backend_->settings()) {
        if (s.category == category) {
            reset(s.key);
        }
    }
}

bool SettingsModel::save() {
    try {
        backend_->save();
    } catch (...) {
        problem_ = explain_current_exception();
        return false;
    }
    dirty_ = false;
    problem_.reset();
    return true;
}

} // namespace dq8::launcher
