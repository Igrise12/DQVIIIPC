// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// SPDX-FileCopyrightText: PCSX2 Dev Team
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: the DMA chain walker and the
// scratchpad transfers of ps2xRuntime/src/lib/ps2_memory.cpp. DMA address resolution follows PCSX2
// (GPL-3.0-or-later) dmaGetAddr in pcsx2/Dmac.h at 5b6e6e2.
#pragma once

#include "runtime/dma/register_block.h"
#include "runtime/machine/events.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine_services.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The DMA controller (architecture §5.3): channel registers at 0x10008000-0x1000D4FF, D_CTRL..D_STADR at
// 0x1000E000, D_ENABLER/D_ENABLEW at 0x1000F520/0x1000F590. Setting CHCR.STR runs the whole transfer at
// once: normal, chain (every source tag id: refe, cnt, next, ref, refs, call, ret, end, with the ASR stack,
// TTE and TIE) and, for the scratchpad channels, destination chains and interleave. The data is delivered
// synchronously to VIF0, VIF1, the GIF's PATH3 or the scratchpad, but STR stays set and the channel's D_STAT
// bit is raised only at a completion event `dma_per_qword` cycles per quadword later; if the target is
// holding data back (a stalled VIF, a masked PATH3), completion waits until it lets go. The SIF channels
// 5 and 6 only store their registers: SIF transfers belong to the IOP link (session C).
namespace dq8::rt {

class Vif;
class Gif;

class Dmac final : public IDevice, public IRegisterBlock {
  public:
    static constexpr std::uint32_t base = 0x10008000u; // block base: channel 0
    // CHCR bits.
    static constexpr std::uint32_t chcr_dir = 1u << 0;
    static constexpr std::uint32_t chcr_tte = 1u << 6;
    static constexpr std::uint32_t chcr_tie = 1u << 7;
    static constexpr std::uint32_t chcr_str = 1u << 8;
    // D_CTRL.DMAE and D_ENABLEW.CPND.
    static constexpr std::uint32_t ctrl_dmae = 1u << 0;
    static constexpr std::uint32_t enable_cpnd = 1u << 16;
    // The longest chain walked before the machine stops (a looping chain is a guest or model bug).
    static constexpr std::uint64_t max_tags = 1u << 20;

    struct Channel {
        std::uint32_t chcr = 0;
        std::uint32_t madr = 0;
        std::uint32_t qwc = 0;
        std::uint32_t tadr = 0;
        std::uint32_t asr0 = 0;
        std::uint32_t asr1 = 0;
        std::uint32_t sadr = 0;
        bool busy = false;    // STR is set and the transfer ran; completion pending
        bool waiting = false; // STR is set but DMAE or CPND kept it from starting
        bool held = false;    // the target holds data; completion waits for it
        EventId completion = 0;
        Cycles finish_at = 0;
    };

    struct Stats {
        std::uint64_t transfers = 0;
        std::uint64_t qwords = 0;
        std::uint64_t tags = 0;
    };

    Dmac(Vif& vif0, Vif& vif1, Gif& gif) noexcept : vif0_(vif0), vif1_(vif1), gif_(gif) {}

    std::string_view name() const override { return "dmac"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) override;
    bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                 const MmioContext& at) override;

    const Channel& channel(DmaChannel c) const noexcept { return channels_[static_cast<unsigned>(c)]; }
    const Stats& stats(DmaChannel c) const noexcept { return stats_[static_cast<unsigned>(c)]; }

  private:
    // A transfer's run: what it moved, for the completion time.
    struct Run {
        std::uint64_t qwords = 0;
        std::uint64_t tags = 0;
    };

    static std::optional<unsigned> channel_at(std::uint32_t offset) noexcept;
    void start(unsigned ch);
    void kick_waiting();
    void complete(unsigned ch);
    void release(unsigned ch);
    bool target_holding(unsigned ch) const;

    // Guest memory as DMA sees it: bit 31 (or a 0x7xxxxxxx address) selects the scratchpad, everything
    // else is a physical RAM address. Copies `qwc` quadwords out of / into guest memory.
    void read_qwords(std::uint32_t addr, std::uint32_t qwc, std::vector<std::uint8_t>& out, unsigned ch);
    void write_qwords(std::uint32_t addr, std::span<const std::uint8_t> bytes, unsigned ch);
    std::uint8_t* qword_ptr(std::uint32_t addr, unsigned ch);
    [[noreturn]] void fail(unsigned ch, const std::string& what);

    void deliver(unsigned ch, std::span<const std::uint8_t> bytes);
    Run run_normal(unsigned ch);
    Run run_source_chain(unsigned ch);
    Run run_spr_from(unsigned ch);
    Run run_spr_to(unsigned ch);
    Run run_vif1_to_memory(unsigned ch);

    Vif& vif0_;
    Vif& vif1_;
    Gif& gif_;
    MachineServices* machine_ = nullptr;
    std::optional<RegisterWindow> chan_window_;
    std::optional<RegisterWindow> ctrl_window_;
    std::optional<RegisterWindow> enabler_window_;
    std::optional<RegisterWindow> enablew_window_;

    std::array<Channel, dma_channel_count> channels_{};
    std::array<Stats, dma_channel_count> stats_{};
    std::uint32_t ctrl_ = 0;
    std::uint32_t pcr_ = 0;
    std::uint32_t sqwc_ = 0;
    std::uint32_t rbsr_ = 0;
    std::uint32_t rbor_ = 0;
    std::uint32_t stadr_ = 0;
    std::uint32_t enable_ = 0;
    std::vector<std::uint8_t> buffer_;
};

} // namespace dq8::rt
