// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop.h"

#include "runtime/ee/guest_stop.h"
#include "runtime/io/disc_source.h"
#include "runtime/iop/iop_exec.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/iop_modules.h"
#include "runtime/iop/iop_rpc.h"
#include "runtime/iop/services/services.h"
#include "runtime/iop/sif_link.h"
#include "runtime/iop/sif_protocol.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/events.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dq8::rt {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// The last path component without device and version, upper case: "cdrom0:\MODULES\IOPRP300.IMG;1" ->
// "IOPRP300.IMG".
std::string file_component(std::string_view path) {
    if (const auto colon = path.find(':'); colon != std::string_view::npos) {
        path.remove_prefix(colon + 1);
    }
    if (const auto sep = path.find_last_of("\\/"); sep != std::string_view::npos) {
        path.remove_prefix(sep + 1);
    }
    if (const auto semi = path.find(';'); semi != std::string_view::npos) {
        path = path.substr(0, semi);
    }
    return upper(path);
}

std::string printable(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const std::uint8_t b : bytes) {
        out.push_back(b >= 0x20 && b < 0x7F ? static_cast<char>(b) : (b == 0 ? ' ' : '?'));
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

} // namespace

struct Iop::Impl final : public ISifCommandSink, public IVblankListener {
    explicit Impl(IopConfig cfg) : config(std::move(cfg)), registry(config.modules), link(memory) {
        if (config.disc == nullptr) {
            throw std::invalid_argument("the IOP needs a disc source");
        }
        for (const IopModuleSpec& spec : registry.specs()) {
            for (const std::string& service : spec.services) {
                if (service == "padman.v1" && config.input == nullptr) {
                    throw std::invalid_argument("IOP module " + spec.name +
                                                " needs an input source for padman.v1");
                }
            }
        }
        deps.disc = config.disc;
        deps.input = config.input;
        deps.load_module = [this](std::string_view path, std::span<const std::uint8_t> args,
                                  std::uint64_t request) { return load_module(path, args, request); };
        deps.send_command = [this](std::uint32_t cid, std::uint32_t opt, std::span<const std::uint8_t> body,
                                   std::vector<EeWrite> writes, Cycles ready_at) {
            send_command(cid, opt, body, std::move(writes), ready_at);
        };
    }

    void attach(MachineServices& m) {
        machine = &m;
        rpc = std::make_unique<IopRpcLayer>(m, link, memory, config.disc);
        exec = std::make_unique<IopExec>(m, memory, link, *rpc, config.devices, config.strict_imports);
        link.attach(m, *this);
        m.add_vblank_listener(VblankPhase::iop, *this);
        exec->connect_devices();
    }

    // Power-on: the IOP has booted with the standard services and nothing else.
    void reset() {
        cancel_reboot();
        exec->reset();
        memory.reset();
        link.reset();
        rpc->clear();
        registry.reset();
        iop_sregs.fill(0);
        boot_standard();
    }

    void boot_standard() {
        for (const std::string_view name : standard_iop_services()) {
            for (auto& server : make_iop_service(name, deps)) {
                rpc->add(std::move(server), {});
            }
        }
    }

    void on_command(std::span<const std::uint8_t> packet) override {
        const std::uint32_t size = sif::packet_size(packet);
        const std::uint32_t cid = sif::load32(packet, sif::hdr_cid);
        if (size < sif::header_size || size > packet.size()) {
            fail(StopReason::guest_error, "a command packet (" + hex32(cid) + ") gives its size as " +
                                              std::to_string(size) + " bytes");
        }
        switch (cid) {
        case sif::cid_change_receive:
            link.set_ee_receive_buffer(sif::load32(packet, sif::change_receive_address));
            break;
        case sif::cid_set_sreg: {
            const std::uint32_t index = sif::load32(packet, sif::set_sreg_index);
            if (index >= iop_sregs.size()) {
                fail(StopReason::guest_error,
                     "set-software-register command for register " + std::to_string(index));
            }
            iop_sregs[index] = sif::load32(packet, sif::set_sreg_value);
            break;
        }
        case sif::cid_init:
            switch (sif::load32(packet, sif::hdr_opt)) {
            case sif::init_opt_command:
                // The EE's command layer gives its receive buffer.
                link.set_ee_receive_buffer(sif::load32(packet, sif::change_receive_address));
                link.raise_smflg(sif::flag_cmd_ready);
                break;
            case sif::init_opt_rpc: {
                // The EE's RPC layer waits until the IOP sets the EE's software register 0.
                std::uint8_t body[8] = {};
                sif::store32(body, 0, sif::rpc_ready_sreg);
                sif::store32(body, 4, sif::rpc_ready_value);
                send_command(sif::cid_set_sreg, 0, body, {},
                             machine->clock().now() + machine->cost().rpc_reply(0));
                break;
            }
            default:
                fail(StopReason::unsupported,
                     "init command with option word " + hex32(sif::load32(packet, sif::hdr_opt)));
            }
            break;
        case sif::cid_reset:
            begin_reboot(packet);
            break;
        case sif::cid_rpc_bind:
            rpc->bind(packet);
            break;
        case sif::cid_rpc_call:
            rpc->call(packet);
            break;
        case sif::cid_rpc_rdata:
            rpc->rdata(packet);
            break;
        case sif::cid_rpc_end:
            // The end of a request the interpreted IOP made of an EE server.
            if (!exec->has_modules()) {
                fail(StopReason::guest_error, "an RPC end packet arrived, but no IOP module made a request");
            }
            exec->on_rpc_end(packet);
            break;
        default:
            fail(StopReason::unsupported, "command " + hex32(cid) + " (" +
                                              std::string(sif::command_name(cid)) + ") has no IOP handler");
        }
    }

    void on_vblank_start(std::uint64_t field) override {
        for (const IopRpcLayer::Server* s : rpc->servers()) {
            if (auto* client = dynamic_cast<IopVblankClient*>(s->impl.get())) {
                client->on_iop_vblank(*machine, field);
            }
        }
    }

    // The reset command: the IOP reboots with the update image. The image is accepted, not executed: every
    // loaded module and its services go, the standard services come back, and the boot flags rise once the
    // image would have been read from the disc.
    void begin_reboot(std::span<const std::uint8_t> packet) {
        const std::uint32_t arg_length =
            std::min(sif::load32(packet, sif::reset_arg_length), sif::reset_arg_max);
        const std::uint32_t mode = sif::load32(packet, sif::reset_mode);
        const auto arg_bytes = packet.size() >= sif::reset_arg + arg_length
                                   ? packet.subspan(sif::reset_arg, arg_length)
                                   : std::span<const std::uint8_t>{};
        const std::string arg = printable(arg_bytes);
        std::uint64_t image_bytes = 0;
        bool image_named = config.reset_image.empty() || arg.empty();
        std::size_t pos = 0;
        while (pos < arg.size()) {
            const std::size_t end = std::min(arg.find(' ', pos), arg.size());
            const std::string token = arg.substr(pos, end - pos);
            pos = end + 1;
            if (token.empty()) {
                continue;
            }
            const std::string device =
                token.find(':') == std::string::npos ? "" : upper(token.substr(0, token.find(':')));
            if (device == "CDROM0" || device == "CDROM") {
                const auto file = config.disc->find(token.substr(token.find(':') + 1));
                if (!file) {
                    fail(StopReason::guest_error,
                         "the IOP is reset with " + token + ", which is not on the disc");
                }
                image_bytes += file->size;
            }
            if (!config.reset_image.empty() && file_component(token) == file_component(config.reset_image)) {
                image_named = true;
            }
        }
        if (!image_named) {
            fail(StopReason::unsupported,
                 "the IOP is reset with '" + arg + "'; the region profile expects " + config.reset_image);
        }
        machine->emit(MachineEvent{"iop_reset", {}}.add("image", arg).add("mode", std::uint64_t{mode}));

        cancel_reboot();
        exec->reset();
        link.discard_to_ee();
        rpc->clear();
        registry.reset();
        memory.reset();
        iop_sregs.fill(0);
        const Cycles when = machine->clock().now() + machine->cost().cdvd_read(image_bytes);
        reboot_event = machine->events().schedule(when, "iop_reboot", [this](Cycles) { finish_reboot(); });
    }

    void finish_reboot() {
        reboot_event = 0;
        boot_standard();
        link.set_smcom(SifLink::command_buffer);
        link.raise_smflg(sif::boot_flags);
    }

    void cancel_reboot() {
        if (reboot_event != 0 && machine != nullptr) {
            machine->events().cancel(reboot_event);
        }
        reboot_event = 0;
    }

    // The loadfile server's request: register the services the profile lists for the module, or run the
    // module on the interpreted IOP (nullopt: the reply follows its start).
    std::optional<std::int32_t> load_module(std::string_view path, std::span<const std::uint8_t> args,
                                            std::uint64_t request) {
        const std::string name = IopModuleRegistry::module_name(path);
        const auto colon = path.find(':');
        const std::string device = colon == std::string_view::npos ? "" : upper(path.substr(0, colon));
        std::optional<DiscFile> file;
        if (device == "CDROM0" || device == "CDROM") {
            file = config.disc->find(path.substr(colon + 1));
            if (!file) {
                fail(StopReason::guest_error,
                     "loadfile: module " + std::string(path) + " is not on the disc");
            }
        } else if (device != "ROM0" && device != "ROM") {
            fail(StopReason::unsupported,
                 "loadfile: module path " + std::string(path) + " names no supported device");
        }
        const IopModuleSpec* spec = registry.find(name);
        if (spec == nullptr) {
            fail(StopReason::unsupported, "loadfile: module " + name + " (" + std::string(path) +
                                              ") is not in the region profile's [iop] table");
        }
        if (const auto* loaded = registry.loaded(name)) {
            return loaded->id;
        }
        if (spec->services.size() == 1 && spec->services.front() == interpret_service) {
            if (!file) {
                fail(StopReason::unsupported, "loadfile: module " + name + " is marked interpret but " +
                                                  std::string(path) + " is not a disc file");
            }
            return load_interpreted(name, path, *file, args, request);
        }
        std::string services;
        for (const std::string& service : spec->services) {
            for (auto& server : make_iop_service(service, deps)) {
                if (rpc->find(server->sid()) == nullptr) {
                    rpc->add(std::move(server), name);
                }
            }
            services += (services.empty() ? "" : ",") + service;
        }
        const std::int32_t id = registry.note_loaded(name);
        machine->emit(MachineEvent{"iop_module", {}}
                          .add("name", name)
                          .add("id", static_cast<std::uint64_t>(id))
                          .add("services", services)
                          .add("args", printable(args))
                          .add("mode", std::string("native"))
                          .add("base", std::uint64_t{0})
                          .add("size", std::uint64_t{0})
                          .add("start", std::int64_t{0}));
        return id;
    }

    std::optional<std::int32_t> load_interpreted(const std::string& name, std::string_view path,
                                                 const DiscFile& file, std::span<const std::uint8_t> args,
                                                 std::uint64_t request) {
        const auto sectors =
            static_cast<std::uint32_t>((file.size + IDiscSource::sector_size - 1) / IDiscSource::sector_size);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sectors) * IDiscSource::sector_size);
        config.disc->read_sectors(file.lsn, sectors, bytes);
        bytes.resize(static_cast<std::size_t>(file.size));
        const std::int32_t id = registry.note_loaded(name);
        const std::string shown_args = printable(args);
        exec->load_module(name, id, bytes, path, args,
                          [this, request, shown_args](const IopExec::Module& m, std::int32_t result) {
                              machine->emit(MachineEvent{"iop_module", {}}
                                                .add("name", m.name)
                                                .add("id", static_cast<std::uint64_t>(m.id))
                                                .add("services", std::string(interpret_service))
                                                .add("args", shown_args)
                                                .add("mode", std::string(interpret_service))
                                                .add("base", std::uint64_t{m.base})
                                                .add("size", std::uint64_t{m.size})
                                                .add("start", std::int64_t{result}));
                              rpc->complete(request, RpcReply{iop_services::loadfile_reply(m.id, result),
                                                              {},
                                                              exec->now() + machine->cost().rpc_reply(8)});
                          });
        return std::nullopt;
    }

    // A command packet for the EE: header (size, cid, option word), then `body`.
    void send_command(std::uint32_t cid, std::uint32_t opt, std::span<const std::uint8_t> body,
                      std::vector<EeWrite> writes, Cycles ready_at) {
        const auto size = static_cast<std::uint32_t>(sif::header_size + body.size());
        if (size > sif::max_packet_size) {
            throw std::logic_error("IOP command packet of " + std::to_string(size) + " bytes");
        }
        std::vector<std::uint8_t> packet(size, 0);
        sif::store32(packet, sif::hdr_sizes, size);
        sif::store32(packet, sif::hdr_cid, cid);
        sif::store32(packet, sif::hdr_opt, opt);
        std::copy(body.begin(), body.end(), packet.begin() + sif::header_size);
        link.send_to_ee(std::move(packet), std::move(writes), ready_at);
    }

    [[noreturn]] void fail(StopReason reason, std::string message) const {
        StopInfo info;
        info.reason = reason;
        info.message = "iop: " + std::move(message);
        machine->stop(std::move(info));
        std::abort(); // stop() never returns
    }

    IopConfig config;
    IopModuleRegistry registry;
    IopMemory memory;
    SifLink link;
    IopServiceDeps deps;
    MachineServices* machine = nullptr;
    std::unique_ptr<IopRpcLayer> rpc;
    std::unique_ptr<IopExec> exec;
    std::array<std::uint32_t, sif::software_reg_count> iop_sregs{};
    EventId reboot_event = 0;
};

Iop::Iop(IopConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Iop::~Iop() = default;

void Iop::attach(MachineServices& machine) {
    impl_->attach(machine);
}

void Iop::reset() {
    if (impl_->machine == nullptr) {
        throw std::logic_error("Iop::reset before attach");
    }
    impl_->reset();
}

std::span<const std::uint8_t> Iop::ram() const {
    return {impl_->memory.data(), IopMemory::size};
}

IopExec& Iop::interpreter() {
    if (!impl_->exec) {
        throw std::logic_error("Iop::interpreter before attach");
    }
    return *impl_->exec;
}

} // namespace dq8::rt
