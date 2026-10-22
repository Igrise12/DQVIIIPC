// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Pad port assignment by device identity (sdl_ports.h). M5 session C.
#include "runtime/platform/sdl/sdl_ports.h"

namespace dq8::rt::sdl {

void PortAssigner::pin(const std::string& guid, unsigned port) {
    if (port < port_count && !guid.empty()) {
        pins_[guid] = port;
    }
}

bool PortAssigner::pinned_elsewhere(unsigned port, const std::string& guid) const {
    for (const auto& [pinned_guid, pinned_port] : pins_) {
        if (pinned_port == port && pinned_guid != guid) {
            return true;
        }
    }
    return false;
}

std::optional<unsigned> PortAssigner::connect(std::uint32_t instance, const std::string& guid,
                                              const std::string& serial) {
    if (const auto held = port_of(instance)) {
        return held; // already connected (SDL reports a device once, but be safe)
    }
    const std::string identity = serial.empty() ? guid : guid + "/" + serial;
    const auto take = [&](unsigned port) {
        ports_[port].instance = instance;
        ports_[port].identity = identity;
        return std::optional<unsigned>(port);
    };
    const auto free = [&](unsigned port) { return !ports_[port].instance.has_value(); };

    if (const auto pin = pins_.find(guid); pin != pins_.end() && free(pin->second)) {
        return take(pin->second);
    }
    for (unsigned port = 0; port < port_count; ++port) {
        if (free(port) && ports_[port].identity == identity) {
            return take(port);
        }
    }
    for (unsigned port = 0; port < port_count; ++port) {
        if (free(port) && ports_[port].identity.empty() && !pinned_elsewhere(port, guid)) {
            return take(port);
        }
    }
    for (unsigned port = 0; port < port_count; ++port) {
        if (free(port) && !pinned_elsewhere(port, guid)) {
            return take(port);
        }
    }
    for (unsigned port = 0; port < port_count; ++port) {
        if (free(port)) {
            return take(port);
        }
    }
    return std::nullopt;
}

std::optional<unsigned> PortAssigner::disconnect(std::uint32_t instance) {
    const auto port = port_of(instance);
    if (port) {
        ports_[*port].instance.reset(); // the identity stays: the port is kept for this device
    }
    return port;
}

std::optional<std::uint32_t> PortAssigner::instance_at(unsigned port) const {
    return port < port_count ? ports_[port].instance : std::nullopt;
}

std::optional<unsigned> PortAssigner::port_of(std::uint32_t instance) const {
    for (unsigned port = 0; port < port_count; ++port) {
        if (ports_[port].instance == instance) {
            return port;
        }
    }
    return std::nullopt;
}

} // namespace dq8::rt::sdl
