// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/pad_state.h"

#include <array>
#include <cstdint>
#include <mutex>

// Rumble out of the machine (architecture §8.1): the pad service reports the motors on the machine thread;
// the window's main thread hands the latest state of each port to the platform (IPlatform is main-thread
// only). Nothing flows back into the guest.
namespace dq8::rt {

class IPlatform;

class LivePadFeedback final : public IPadFeedbackSink {
  public:
    // Machine thread.
    void actuators(unsigned port, std::uint64_t vblank, const PadActuators& state) override;
    // Main thread: drives the motors of every port whose state changed since the last call.
    void apply(IPlatform& platform);

    // The platform's strengths for a motor state: the large motor on the low-frequency channel (0..255 scaled
    // to 0..65535), the small one on the high-frequency channel (on or off).
    static std::uint16_t low_frequency(const PadActuators& state) noexcept;
    static std::uint16_t high_frequency(const PadActuators& state) noexcept;

  private:
    struct Port {
        PadActuators state;
        bool changed = false;
    };
    std::mutex mutex_;
    std::array<Port, 2> ports_{};
};

} // namespace dq8::rt
