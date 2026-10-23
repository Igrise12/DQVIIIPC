// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

// Which gamepad drives which pad port (M5 session C, architecture §8.1 "hot-plug"), as plain logic the tests
// drive without SDL. A port remembers the device that last held it, so a gamepad unplugged and plugged in
// again gets its port back even when another one arrived meanwhile. A device is known by its identity: its
// SDL GUID (vendor, product, version: the same for two pads of one model) plus its serial number when SDL
// reports one.
//
// A device that arrives takes, in this order: the port its GUID is pinned to (the input profile's `port`),
// when free; a free port it held before; a free port nobody held yet and no other GUID is pinned to; any free
// port no other GUID is pinned to; any free port. With every port taken it waits and gets no port (it is not
// promoted when a port frees up, which would take a replugged pad's port away).
namespace dq8::rt::sdl {

class PortAssigner {
  public:
    static constexpr unsigned port_count = 2;

    // Pins a GUID to a port (0 or 1); a later pin of the same GUID replaces it.
    void pin(const std::string& guid, unsigned port);
    // Device `instance` arrived; returns the port it drives, if any.
    std::optional<unsigned> connect(std::uint32_t instance, const std::string& guid,
                                    const std::string& serial);
    // Device `instance` left; returns the port it drove, if any.
    std::optional<unsigned> disconnect(std::uint32_t instance);
    // The device driving `port`, if any.
    std::optional<std::uint32_t> instance_at(unsigned port) const;
    // The port device `instance` drives, if any.
    std::optional<unsigned> port_of(std::uint32_t instance) const;

  private:
    struct Port {
        std::optional<std::uint32_t> instance;
        std::string identity; // of the device that holds it, or held it last; empty: never held
    };
    bool pinned_elsewhere(unsigned port, const std::string& guid) const;

    std::array<Port, port_count> ports_;
    std::map<std::string, unsigned> pins_; // GUID -> port
};

} // namespace dq8::rt::sdl
