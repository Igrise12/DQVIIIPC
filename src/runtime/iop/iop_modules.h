// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/iop.h"
#include "runtime/iop/iop_server.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The native service catalogue and the game's module table (ADR-0011). Which services stand in for which
// module is the region profile's [iop] table (IopModuleSpec); this file only knows the services by name:
//   fileio, iopheap, loadfile, cdvdfsv   the standard services every IOP boot provides
//   padman.v1                            the pad server of the old pad library (two server ids)
//   mcserv.v1                            the memory-card server: a card present and unformatted
//   null:<sid>                           acknowledges every call with zeros (sound servers until M4)
//   interpret                            M4 (ADR-0014): the module itself runs on the interpreted IOP; alone
//                                        in its row
namespace dq8::rt {

class IDiscSource;
class IInputSource;
class MachineServices;

// A server that also works at every vblank (in the IOP vblank phase): the pad server's DMA frame.
class IopVblankClient {
  public:
    virtual ~IopVblankClient() = default;
    virtual void on_iop_vblank(MachineServices& machine, std::uint64_t field) = 0;
};

// What services need beyond IopCallContext.
struct IopServiceDeps {
    const IDiscSource* disc = nullptr;
    IInputSource* input = nullptr;
    // M4 session D: the memory cards (null: no card), the virtual clock, the pad feedback sink.
    IMemoryCardStore* cards[2] = {nullptr, nullptr};
    const VirtualRtc* rtc = nullptr;
    IPadFeedbackSink* pad_feedback = nullptr;
    // loadfile: load a module by path with its argument block; returns the module id or a negative IOP error,
    // or nullopt when the module runs interpreted and its start has not returned yet: the Iop then answers
    // RPC request `request` itself (IopRpcLayer::complete) once it has.
    std::function<std::optional<std::int32_t>(std::string_view path, std::span<const std::uint8_t> args,
                                              std::uint64_t request)>
        load_module;
    // Sends a command packet of the given id and option word to the EE (the header is filled in; `body`
    // follows it), after `writes` land, no earlier than `ready_at`: what a server's own IOP thread sends
    // outside the RPC reply (fileio's completion records).
    std::function<void(std::uint32_t cid, std::uint32_t opt, std::span<const std::uint8_t> body,
                       std::vector<EeWrite> writes, Cycles ready_at)>
        send_command;
};

// True for a catalogue name (including any well-formed "null:<sid>" and "interpret").
bool is_iop_service(std::string_view name);
inline constexpr std::string_view interpret_service = "interpret";
// The servers a catalogue name stands for ("interpret" stands for none: the module registers its own);
// `deps` must outlive them. std::invalid_argument for an unknown name.
std::vector<std::unique_ptr<IopRpcServer>> make_iop_service(std::string_view name,
                                                            const IopServiceDeps& deps);
// The services an IOP boot (and every reboot) provides before any module is loaded.
std::span<const std::string_view> standard_iop_services();

class IopModuleRegistry {
  public:
    struct Loaded {
        std::string name;
        std::int32_t id = 0;
    };

    // Throws std::invalid_argument for an empty or duplicate module name or an unknown service name.
    explicit IopModuleRegistry(std::vector<IopModuleSpec> specs);

    // The module name a load path stands for: the last path component, upper case, without its version and
    // extension. "cdrom0:\MODULES\PADMAN.IRX;1" and "rom0:PADMAN" are both "PADMAN".
    static std::string module_name(std::string_view path);

    const IopModuleSpec* find(std::string_view name) const;
    const std::vector<IopModuleSpec>& specs() const noexcept { return specs_; }

    // Modules loaded since the last IOP boot, in load order.
    const Loaded* loaded(std::string_view name) const;
    const std::vector<Loaded>& loaded() const noexcept { return loaded_; }
    std::int32_t note_loaded(std::string name);
    void reset();

  private:
    std::vector<IopModuleSpec> specs_;
    std::vector<Loaded> loaded_;
    std::int32_t next_id_ = first_module_id;
    static constexpr std::int32_t first_module_id = 0x20;
};

} // namespace dq8::rt
