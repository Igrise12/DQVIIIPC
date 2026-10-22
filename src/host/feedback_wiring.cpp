// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M4 session D: pad rumble. With --window and --rumble on (the default) the pad service's motor changes go to
// a LivePadFeedback, which the window loop hands to the platform's gamepads (IPlatform::set_rumble) once per
// pass. Headless, or with --rumble off, nothing is connected: the pad_rumble events still record what the
// game asked for, and nothing about rumble ever reaches the guest.
#include "host/m4_wiring.h"
#include "runtime/io/pad_feedback.h"

#include <memory>
#include <string_view>

namespace dq8::host {

namespace {

// Owns the feedback sink for as long as the IOP that points at it.
class FeedbackDevice final : public rt::IDevice {
  public:
    std::string_view name() const override { return "pad-feedback"; }
    void attach(rt::MachineServices& /*machine*/) override {}
    void reset() override {}
    rt::LivePadFeedback feedback;
};

} // namespace

void wire_feedback(M4Wiring& w) {
    if (!w.options.rumble || w.platform == nullptr) {
        return;
    }
    auto device = std::make_unique<FeedbackDevice>();
    rt::LivePadFeedback* feedback = &device->feedback;
    rt::IPlatform* platform = w.platform;
    w.iop.pad_feedback = feedback;
    w.on_frame.push_back([feedback, platform] { feedback->apply(*platform); });
    // The motors stop when the run does.
    w.on_stop.push_back([platform] {
        platform->set_rumble(0, 0, 0);
        platform->set_rumble(1, 0, 0);
    });
    w.devices.push_back(std::move(device));
}

} // namespace dq8::host
