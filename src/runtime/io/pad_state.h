// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstdint>

// The virtual DualShock 2 (architecture §8.1). Input sources (scripted, live from the window, replayed)
// produce one PadState per port per vblank on the machine thread; the pad service turns it into the frame the
// game's pad library reads. Sampling at a fixed point of each vblank is what makes scripted and recorded runs
// reproducible.
namespace dq8::rt {

// Buttons, set = pressed. Bit positions follow the DualShock 2's button bytes (the pad service inverts them
// to the controller's active-low wire form).
enum PadButton : std::uint16_t {
    pad_select = 1u << 0,
    pad_l3 = 1u << 1,
    pad_r3 = 1u << 2,
    pad_start = 1u << 3,
    pad_up = 1u << 4,
    pad_right = 1u << 5,
    pad_down = 1u << 6,
    pad_left = 1u << 7,
    pad_l2 = 1u << 8,
    pad_r2 = 1u << 9,
    pad_l1 = 1u << 10,
    pad_r1 = 1u << 11,
    pad_triangle = 1u << 12,
    pad_circle = 1u << 13,
    pad_cross = 1u << 14,
    pad_square = 1u << 15,
};

struct PadState {
    bool connected = true;
    std::uint16_t buttons = 0;                               // PadButton bits
    std::uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80; // sticks, 0x80 = centre
    // Pressure, 0..255, in the DualShock 2 report order: right, left, up, down, triangle, circle, cross,
    // square, L1, R1, L2, R2.
    std::array<std::uint8_t, 12> pressure{};
};

class IInputSource {
  public:
    virtual ~IInputSource() = default;
    // The state of `port` (0 or 1) for field `vblank`. Called on the machine thread once per port per vblank
    // in the IOP vblank phase; a scripted or replayed source must return the same state for the same
    // arguments.
    virtual PadState sample(unsigned port, std::uint64_t vblank) = 0;
};

// The DualShock 2's two motors as the game drives them (M4 session D, architecture §8.1): the small motor is
// on or off, the large one has a strength.
struct PadActuators {
    bool small = false;
    std::uint8_t large = 0;
    friend bool operator==(const PadActuators&, const PadActuators&) = default;
};

// Where the pad service reports actuator changes, on the machine thread in the IOP vblank phase. Nothing it
// does flows back into the guest: rumble is output only.
class IPadFeedbackSink {
  public:
    virtual ~IPadFeedbackSink() = default;
    virtual void actuators(unsigned port, std::uint64_t vblank, const PadActuators& state) = 0;
};

} // namespace dq8::rt
