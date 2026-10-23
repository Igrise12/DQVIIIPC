// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/settings.h"
#include "model/errors.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Settings pages, generated from the schema and its string table (ADR-0006, architecture.md §9.2): one
// page per category in the schema's order, one row per setting, the widget chosen by the setting's type and
// range. A new schema row needs no code here. Edits go to the user layer of the settings store and are saved
// to the user's settings file, which Play passes to dq8-game with --config.
namespace dq8::launcher {

// What the pages need from the settings store. StoreSettings is the real one (core::settings); tests and the
// preview use their own.
class SettingsBackend {
  public:
    virtual ~SettingsBackend() = default;
    virtual const std::vector<core::settings::Setting>& settings() const = 0;
    // A string-table text, or the key itself when the table lacks it.
    virtual std::string text(std::string_view string_key) const = 0;
    virtual core::settings::Value value(std::string_view key) const = 0; // the effective value
    virtual bool user_set(std::string_view key) const = 0;               // the user layer holds the key
    virtual void set(std::string_view key, const core::settings::Value& value) = 0; // the user layer
    virtual void reset(std::string_view key) = 0; // drop it from the user layer
    virtual void save() = 0;                      // throws core::Error
    virtual std::filesystem::path file() const = 0;
    virtual std::vector<std::string> warnings() const = 0;
};

// Values in memory over a given list of settings: the tests and the launcher's --preview.
class MemorySettings final : public SettingsBackend {
  public:
    MemorySettings(std::vector<core::settings::Setting> settings,
                   std::map<std::string, std::string, std::less<>> strings, std::filesystem::path file = {});

    const std::vector<core::settings::Setting>& settings() const override { return settings_; }
    std::string text(std::string_view string_key) const override;
    core::settings::Value value(std::string_view key) const override;
    bool user_set(std::string_view key) const override { return user_.find(key) != user_.end(); }
    void set(std::string_view key, const core::settings::Value& value) override;
    void reset(std::string_view key) override;
    void save() override; // counts; throws Error(io_write) after fail_saves(true)
    std::filesystem::path file() const override { return file_; }
    std::vector<std::string> warnings() const override { return {}; }

    int saves() const noexcept { return saves_; }
    void fail_saves(bool fail) noexcept { fail_saves_ = fail; }

  private:
    const core::settings::Setting& find(std::string_view key) const;

    std::vector<core::settings::Setting> settings_;
    std::map<std::string, std::string, std::less<>> strings_;
    std::map<std::string, core::settings::Value, std::less<>> user_;
    std::filesystem::path file_;
    int saves_ = 0;
    bool fail_saves_ = false;
};

// core::settings over the schema and strings found next to the program and the user's settings file. Throws
// what core throws (no schema found, a broken settings file).
std::unique_ptr<SettingsBackend> open_store_settings(const std::filesystem::path& executable_dir,
                                                     std::string_view language = "en");

// "true", "1280", "0.5", "1x": the form a `requires` condition compares against.
std::string format_value(const core::settings::Value& value);
bool value_matches_type(const core::settings::Value& value, core::settings::Type type) noexcept;

enum class Widget {
    checkbox,    // boolean
    slider_int,  // integer with at most 100 steps
    input_int,   // integer with a wider range
    slider_real, // real
    combo,       // choice
    text,        // text
    path,        // path: text plus a folder picker
};

struct Choice {
    std::string value;
    std::string text;
};

struct SettingRow {
    std::string key;
    std::string label;
    std::string description;
    Widget widget = Widget::checkbox;
    core::settings::Value value;
    core::settings::Value default_value;
    std::vector<Choice> choices; // combo
    std::int64_t min = 0;        // slider_int, input_int
    std::int64_t max = 0;
    double min_real = 0.0; // slider_real
    double max_real = 0.0;
    bool advanced = false;
    bool requires_restart = false;
    bool modified = false; // differs from the default
    // "Takes effect the next time the game starts." and the like; empty when nothing needs saying.
    std::string hint;
};

struct SettingsPage {
    std::string category;
    std::string title;
    std::vector<SettingRow> rows;      // the rows to show now
    std::size_t advanced_hidden = 0;   // advanced rows the toggle would add (whose conditions hold)
    std::size_t conditions_hidden = 0; // rows hidden because a `requires` does not hold
};

class SettingsModel {
  public:
    explicit SettingsModel(std::unique_ptr<SettingsBackend> backend);

    // Categories in the order the schema first names them.
    const std::vector<std::string>& categories() const noexcept { return categories_; }
    std::string category_title(std::string_view category) const;
    // The rows of a page; `game_running` adds the "restart the game" hints.
    SettingsPage page(std::string_view category, bool show_advanced, bool game_running) const;

    // A setting's `requires` conditions all hold for the current values (unknown keys never hold).
    bool conditions_hold(const core::settings::Setting& setting) const;

    // Sets a value in the user layer: an integer or real is clamped to its range, a choice must be one of its
    // values. Returns false (and problem() says why) when the value is refused.
    bool set(std::string_view key, const core::settings::Value& value);
    void reset(std::string_view key);
    void reset_page(std::string_view category);

    bool dirty() const noexcept { return dirty_; }
    // Writes the user file; clears dirty() on success.
    bool save();
    const std::optional<Problem>& problem() const noexcept { return problem_; }
    std::filesystem::path file() const { return backend_->file(); }
    std::vector<std::string> warnings() const { return backend_->warnings(); }
    const SettingsBackend& backend() const noexcept { return *backend_; }

  private:
    const core::settings::Setting* find(std::string_view key) const noexcept;
    SettingRow row(const core::settings::Setting& setting, bool game_running) const;

    std::unique_ptr<SettingsBackend> backend_;
    std::vector<std::string> categories_;
    bool dirty_ = false;
    std::optional<Problem> problem_;
};

} // namespace dq8::launcher
