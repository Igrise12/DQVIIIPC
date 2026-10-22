// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 day-0 stub of the settings API (dq8/core/settings.h); M5 session A replaces it.
#include "dq8/core/settings.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace dq8::core::settings {

namespace {

[[noreturn]] void not_implemented(const char* what) {
    throw std::logic_error(std::string("settings: ") + what + " is not implemented (M5 session A)");
}

} // namespace

struct Store::Impl {
    explicit Impl(Schema s) : schema(std::move(s)) {}
    Schema schema;
    std::vector<std::string> warnings;
};

Schema Schema::load(const std::filesystem::path& /*file*/) {
    not_implemented("Schema::load");
}

Schema Schema::parse(std::string_view /*toml_text*/, std::string_view /*origin*/) {
    not_implemented("Schema::parse");
}

int Schema::version() const noexcept {
    return 0;
}

const std::vector<Setting>& Schema::settings() const noexcept {
    static const std::vector<Setting> none;
    return none;
}

const Setting* Schema::find(std::string_view /*key*/) const noexcept {
    return nullptr;
}

StringTable StringTable::load(const std::filesystem::path& /*file*/) {
    not_implemented("StringTable::load");
}

std::string_view StringTable::get(std::string_view key) const noexcept {
    return key;
}

Store::Store(Schema schema) : impl_(std::make_unique<Impl>(std::move(schema))) {}
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

const Schema& Store::schema() const noexcept {
    return impl_->schema;
}

void Store::load(Layer /*layer*/, const std::filesystem::path& /*file*/) {
    not_implemented("Store::load");
}

void Store::load_text(Layer /*layer*/, std::string_view /*toml_text*/, std::string_view /*origin*/) {
    not_implemented("Store::load_text");
}

void Store::save(Layer /*layer*/, const std::filesystem::path& /*file*/) const {
    not_implemented("Store::save");
}

void Store::set_from_text(std::string_view /*assignment*/) {
    not_implemented("Store::set_from_text");
}

void Store::set(Layer /*layer*/, std::string_view /*key*/, Value /*value*/) {
    not_implemented("Store::set");
}

void Store::reset(Layer /*layer*/, std::string_view /*key*/) {
    not_implemented("Store::reset");
}

Value Store::get(std::string_view /*key*/) const {
    not_implemented("Store::get");
}

bool Store::get_bool(std::string_view /*key*/) const {
    not_implemented("Store::get_bool");
}

std::int64_t Store::get_int(std::string_view /*key*/) const {
    not_implemented("Store::get_int");
}

double Store::get_real(std::string_view /*key*/) const {
    not_implemented("Store::get_real");
}

std::string Store::get_string(std::string_view /*key*/) const {
    not_implemented("Store::get_string");
}

Layer Store::source(std::string_view /*key*/) const {
    not_implemented("Store::source");
}

const std::vector<std::string>& Store::warnings() const noexcept {
    return impl_->warnings;
}

std::filesystem::path find_settings_dir(const std::filesystem::path& /*executable_dir*/) {
    not_implemented("find_settings_dir");
}

std::filesystem::path default_user_file(const std::filesystem::path& /*executable_dir*/) {
    not_implemented("default_user_file");
}

} // namespace dq8::core::settings
