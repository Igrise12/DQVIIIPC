// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_memory.h"
#include "runtime/io/pad_state.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/iop_modules.h"
#include "runtime/iop/services/services.h"
#include "runtime/machine/machine_services.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pad server (server ids 0x80000100 and 0x80000101; "Protocol notes", padman). The catalogue calls it
// "padman.v1" after the server id pair; the EE library speaks the command-word protocol: one function number,
// a 128-byte block whose first word is the command, the same block back with the results in it. A port open
// hands the server an EE area of two 128-byte frames; from then on, in the IOP vblank phase of every field,
// the server samples the port from the IInputSource and writes a DualShock 2 frame into the older frame with
// the next counter value, as the IOP pad driver does over SIF DMA. The second server id binds but is never
// called.
namespace dq8::rt::iop_services {

namespace {

constexpr std::uint32_t fn_command = 1;
constexpr std::size_t block_size = 0x80;
constexpr std::size_t cmd_word = 0x00;
constexpr std::size_t cmd_port = 0x04;
constexpr std::size_t cmd_slot = 0x08;
constexpr std::size_t cmd_result = 0x0C;  // open, init, module version
constexpr std::size_t cmd_area = 0x10;    // open: the EE pad area
constexpr std::size_t cmd_result2 = 0x14; // open: the IOP actuator buffer; mode and alignment: accepted
constexpr std::size_t cmd_mode = 0x0C;    // SetMainMode: the mode-table index (0 digital, 1 analog)

constexpr std::uint32_t cmd_open = 0x01;
constexpr std::uint32_t cmd_set_main_mode = 0x06;
constexpr std::uint32_t cmd_set_act_align = 0x08;
constexpr std::uint32_t cmd_init = 0x10;
constexpr std::uint32_t cmd_module_version = 0x12;
constexpr std::uint32_t module_version = 0x0400; // the library needs major version 4
// SetActDirect needs no command: the EE DMAs two 32-byte actuator blocks to this IOP buffer.
constexpr std::uint32_t actuator_buffer_size = 0x40;

// One 128-byte frame of the EE pad area, as the IOP driver fills it.
constexpr std::uint32_t frame_size = 0x80;
constexpr std::size_t frame_data = 0x00;      // the controller's reply, 32 bytes
constexpr std::size_t frame_actuators = 0x30; // 4 bytes per actuator
constexpr std::size_t frame_modes = 0x50;     // u16 mode ids
constexpr std::size_t frame_counter = 0x58;
constexpr std::size_t frame_length = 0x60; // bytes of `data` the library copies out (1..32)
constexpr std::size_t frame_mode_config = 0x64;
constexpr std::size_t frame_mode_id = 0x65;
constexpr std::size_t frame_ready = 0x67;
constexpr std::size_t frame_mode_count = 0x68;
constexpr std::size_t frame_mode_index = 0x69;
constexpr std::size_t frame_actuator_count = 0x6A;
constexpr std::size_t frame_state = 0x70;
constexpr std::size_t frame_request = 0x71;
constexpr std::size_t frame_ok = 0x72;

constexpr std::uint8_t state_disconnected = 0;
constexpr std::uint8_t state_exec_cmd = 4;
constexpr std::uint8_t state_stable = 6;
constexpr std::uint8_t request_complete = 0;
constexpr std::uint8_t request_busy = 1;
// Fields a mode change takes before the pad reports the new mode stable, as a real controller does.
constexpr int mode_switch_fields = 2;

// A DualShock 2 with the digital and analog modes in its table and two actuators. It powers up digital (id
// 0x41: one halfword of buttons) and turns analog (id 0x73: three halfwords) only when the program asks with
// SetMainMode. The game's title code depends on that: it tests for an analog controller and moves on only
// after seeing it not yet analog (M3 bring-up, Docs/milestones/M3-boots.md).
constexpr std::uint8_t id_digital = 0x41;
constexpr std::uint8_t id_analog = 0x73;
constexpr std::uint16_t mode_digital = 0x0004;
constexpr std::uint16_t mode_analog = 0x0007;
constexpr std::uint32_t data_length = 32;
constexpr std::uint8_t actuators[2][4] = {{1, 0, 0, 1}, {1, 1, 1, 1}}; // function, sub, size, current

struct OpenPort {
    std::uint32_t area = 0;      // EE address of the two frames
    std::uint32_t actuators = 0; // IOP address of the actuator buffer
    std::int32_t counter = 0;
    bool analog = false; // the current mode: digital at power-up
    int switching = 0;   // fields left of a mode change
};

struct PadmanState {
    IInputSource* input = nullptr;
    std::map<std::pair<std::uint32_t, std::uint32_t>, OpenPort> open; // (port, slot)
};

void put16(std::vector<std::uint8_t>& f, std::size_t offset, std::uint16_t value) {
    f[offset] = static_cast<std::uint8_t>(value);
    f[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

std::vector<std::uint8_t> make_frame(const PadState& pad, const OpenPort& port) {
    const std::int32_t counter = port.counter;
    const std::uint8_t id = port.analog ? id_analog : id_digital;
    std::vector<std::uint8_t> f(frame_size, 0);
    put32(f, frame_counter, static_cast<std::uint32_t>(counter));
    f[frame_request] = request_complete;
    if (!pad.connected) {
        f[frame_state] = state_disconnected;
        return f;
    }
    const auto buttons = static_cast<std::uint16_t>(~pad.buttons); // active low, high byte first on the wire
    f[frame_data + 0] = 0x00;                                      // transfer ok
    f[frame_data + 1] = id;
    f[frame_data + 2] = static_cast<std::uint8_t>(buttons & 0xFF);
    f[frame_data + 3] = static_cast<std::uint8_t>(buttons >> 8);
    f[frame_data + 4] = pad.rx;
    f[frame_data + 5] = pad.ry;
    f[frame_data + 6] = pad.lx;
    f[frame_data + 7] = pad.ly;
    for (std::size_t i = 0; i < pad.pressure.size(); ++i) {
        f[frame_data + 8 + i] = pad.pressure[i];
    }
    for (std::size_t a = 0; a < 2; ++a) {
        for (std::size_t t = 0; t < 4; ++t) {
            f[frame_actuators + 4 * a + t] = actuators[a][t];
        }
    }
    put16(f, frame_modes, mode_digital);
    put16(f, frame_modes + 2, mode_analog);
    put32(f, frame_length, data_length);
    f[frame_mode_config] = 2;
    f[frame_mode_id] = id;
    f[frame_ready] = 1;
    f[frame_mode_count] = 2;
    f[frame_mode_index] = port.analog ? 1 : 0;
    f[frame_actuator_count] = 2;
    f[frame_state] = port.switching > 0 ? state_exec_cmd : state_stable;
    f[frame_request] = port.switching > 0 ? request_busy : request_complete;
    f[frame_ok] = 1;
    return f;
}

class PadmanServer final : public IopRpcServer, public IopVblankClient {
  public:
    PadmanServer(std::uint32_t sid, std::shared_ptr<PadmanState> state, bool drives_frames)
        : sid_(sid), state_(std::move(state)), drives_frames_(drives_frames) {}

    std::uint32_t sid() const noexcept override { return sid_; }
    std::string_view name() const noexcept override { return sid_ == sid_padman ? "padman" : "padman.ext"; }
    std::uint32_t receive_buffer_size() const noexcept override { return block_size; }

    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        if (call.fno != fn_command) {
            unsupported(call, name(), "not implemented");
        }
        std::vector<std::uint8_t> block(args.begin(), args.end());
        block.resize(block_size, 0);
        const std::uint32_t command = arg32(args, cmd_word);
        if (call.machine.tracing(TraceCategory::rpc)) {
            call.machine.trace(TraceCategory::rpc, "padman command " + std::to_string(command) + " args " +
                                                       std::to_string(arg32(args, 0x04)) + " " +
                                                       std::to_string(arg32(args, 0x08)) + " " +
                                                       std::to_string(arg32(args, 0x0C)) + " " +
                                                       std::to_string(arg32(args, 0x10)));
        }
        switch (command) {
        case cmd_module_version:
            put32(block, cmd_result, module_version);
            break;
        case cmd_init:
            put32(block, cmd_result, 1);
            break;
        case cmd_open:
            put32(block, cmd_result, open(call, args) ? 1 : 0);
            if (const auto it = state_->open.find({arg32(args, cmd_port), arg32(args, cmd_slot)});
                it != state_->open.end()) {
                put32(block, cmd_result2, it->second.actuators);
            }
            break;
        case cmd_set_main_mode:
            if (const auto it = state_->open.find({arg32(args, cmd_port), arg32(args, cmd_slot)});
                it != state_->open.end()) {
                // Only a change of mode switches; the game repeats the request until it sees the mode.
                const bool analog = arg32(args, cmd_mode) == 1;
                if (it->second.analog != analog) {
                    it->second.analog = analog;
                    it->second.switching = mode_switch_fields;
                }
            }
            put32(block, cmd_result2, 1); // accepted; the frames report it busy, then the new mode
            break;
        case cmd_set_act_align:
            put32(block, cmd_result2, 1); // accepted; the next frame reports it complete
            break;
        default:
            unsupported(call, name(), "command " + std::to_string(command) + " not implemented");
        }
        return {std::move(block), {}, 0};
    }

    void on_iop_vblank(MachineServices& machine, std::uint64_t field) override {
        if (!drives_frames_) {
            return;
        }
        for (auto& [key, port] : state_->open) {
            const PadState pad = state_->input->sample(key.first, field);
            ++port.counter;
            // The library reads the frame with the larger counter; write over the other one.
            const std::uint32_t half = static_cast<std::uint32_t>(port.counter) & 1u;
            machine.memory().write_bytes(port.area + half * frame_size, make_frame(pad, port));
            if (port.switching > 0) {
                --port.switching;
            }
        }
    }

  private:
    bool open(IopCallContext& call, std::span<const std::uint8_t> args) {
        const std::uint32_t port = arg32(args, cmd_port);
        const std::uint32_t slot = arg32(args, cmd_slot);
        if (port > 1 || slot != 0) {
            return false;
        }
        OpenPort& p = state_->open[{port, slot}];
        p.area = arg32(args, cmd_area);
        p.counter = 0;
        p.analog = false;
        p.switching = 0;
        if (p.actuators == 0) {
            const auto buffer = call.iop.allocate(actuator_buffer_size);
            if (!buffer) {
                unsupported(call, name(), "no IOP RAM for the actuator buffer");
            }
            p.actuators = *buffer;
        }
        return true;
    }

    std::uint32_t sid_;
    std::shared_ptr<PadmanState> state_;
    bool drives_frames_;
};

} // namespace

std::vector<std::unique_ptr<IopRpcServer>> make_padman_v1(const IopServiceDeps& deps) {
    auto state = std::make_shared<PadmanState>();
    state->input = deps.input;
    std::vector<std::unique_ptr<IopRpcServer>> out;
    out.push_back(std::make_unique<PadmanServer>(sid_padman, state, true));
    out.push_back(std::make_unique<PadmanServer>(sid_padman_ext, state, false));
    return out;
}

} // namespace dq8::rt::iop_services
