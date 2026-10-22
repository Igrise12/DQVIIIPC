// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_modules.h"

#include "runtime/iop/services/services.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

constexpr std::array<std::string_view, 4> standard_services = {"fileio", "iopheap", "loadfile", "cdvdfsv"};

// "null:0x80000701" or "null:2147485441" -> the server id.
std::optional<std::uint32_t> null_server_sid(std::string_view name) {
    constexpr std::string_view prefix = "null:";
    if (name.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    std::string digits(name.substr(prefix.size()));
    if (digits.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t used = 0;
        const unsigned long long value = std::stoull(digits, &used, 0);
        if (used != digits.size() || value == 0 || value > 0xFFFFFFFFull) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

} // namespace

bool is_iop_service(std::string_view name) {
    return std::find(standard_services.begin(), standard_services.end(), name) != standard_services.end() ||
           name == "padman.v1" || name == "mcserv.v1" || name == interpret_service ||
           null_server_sid(name).has_value();
}

std::vector<std::unique_ptr<IopRpcServer>> make_iop_service(std::string_view name,
                                                            const IopServiceDeps& deps) {
    std::vector<std::unique_ptr<IopRpcServer>> out;
    if (name == "fileio") {
        out.push_back(iop_services::make_fileio(deps.disc, deps));
    } else if (name == "iopheap") {
        out.push_back(iop_services::make_iopheap(deps.disc));
    } else if (name == "loadfile") {
        out.push_back(iop_services::make_loadfile(deps.load_module));
    } else if (name == "cdvdfsv") {
        out = iop_services::make_cdvdfsv(deps.disc);
    } else if (name == "padman.v1") {
        out = iop_services::make_padman_v1(deps);
    } else if (name == "mcserv.v1") {
        out.push_back(iop_services::make_mcserv_v1(deps));
    } else if (name == interpret_service) {
        // The interpreted module registers its servers itself.
    } else if (const auto sid = null_server_sid(name)) {
        out.push_back(iop_services::make_null_server(*sid));
    } else {
        throw std::invalid_argument("unknown IOP service '" + std::string(name) + "'");
    }
    return out;
}

std::span<const std::string_view> standard_iop_services() {
    return standard_services;
}

IopModuleRegistry::IopModuleRegistry(std::vector<IopModuleSpec> specs) : specs_(std::move(specs)) {
    std::set<std::string> names;
    for (IopModuleSpec& spec : specs_) {
        spec.name = upper(spec.name);
        if (spec.name.empty()) {
            throw std::invalid_argument("an IOP module spec has an empty name");
        }
        if (!names.insert(spec.name).second) {
            throw std::invalid_argument("IOP module " + spec.name + " is listed twice");
        }
        for (const std::string& service : spec.services) {
            if (!is_iop_service(service)) {
                throw std::invalid_argument("IOP module " + spec.name + " names unknown service '" + service +
                                            "'");
            }
            if (service == interpret_service && spec.services.size() != 1) {
                throw std::invalid_argument("IOP module " + spec.name +
                                            " names 'interpret' with other services; it must stand alone");
            }
        }
    }
}

std::string IopModuleRegistry::module_name(std::string_view path) {
    // Drop the device ("cdrom0:"), then everything up to the last separator, then the version and extension.
    if (const auto colon = path.find(':'); colon != std::string_view::npos) {
        path.remove_prefix(colon + 1);
    }
    if (const auto sep = path.find_last_of("\\/"); sep != std::string_view::npos) {
        path.remove_prefix(sep + 1);
    }
    if (const auto semi = path.find(';'); semi != std::string_view::npos) {
        path = path.substr(0, semi);
    }
    if (const auto dot = path.find('.'); dot != std::string_view::npos) {
        path = path.substr(0, dot);
    }
    return upper(path);
}

const IopModuleSpec* IopModuleRegistry::find(std::string_view name) const {
    const std::string key = upper(name);
    for (const IopModuleSpec& spec : specs_) {
        if (spec.name == key) {
            return &spec;
        }
    }
    return nullptr;
}

const IopModuleRegistry::Loaded* IopModuleRegistry::loaded(std::string_view name) const {
    const std::string key = upper(name);
    for (const Loaded& l : loaded_) {
        if (l.name == key) {
            return &l;
        }
    }
    return nullptr;
}

std::int32_t IopModuleRegistry::note_loaded(std::string name) {
    const std::int32_t id = next_id_++;
    loaded_.push_back(Loaded{upper(name), id});
    return id;
}

void IopModuleRegistry::reset() {
    loaded_.clear();
    next_id_ = first_module_id;
}

} // namespace dq8::rt
