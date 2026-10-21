// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/dma/register_block.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/machine/machine_services.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace dq8::gs {
class IGsSink;
}

// The GIF (architecture §5.3): three paths (PATH1 from VU1 XGKICK, PATH2 from VIF1 DIRECT/DIRECTHL, PATH3
// from GIF DMA and the GIF FIFO) feed one GS. Each path's quadword stream is cut into whole packets (GIFtags
// up to and including the one with EOP) that go to the GS sink in completion order. Before a packet goes out
// it is scanned for the A+D writes the machine must act on: SIGNAL, FINISH and LABEL update the privileged
// registers (and may raise INTC 0); BITBLTBUF, TRXREG and TRXDIR = 1 arm a GS->host readback that VIF1 FIFO
// reads and VIF1 to-memory DMA drain through the sink. PATH3 can be masked (VIF1 MSKPATH3, GIF_MODE.M3R):
// its completed packets then wait, in order, until the mask is lifted.
namespace dq8::rt {

class GsPrivDevice;

class Gif final : public IDevice, public IRegisterBlock {
  public:
    static constexpr std::uint32_t regs_base = 0x10003000u;
    static constexpr std::uint32_t fifo_base = 0x10006000u;
    static constexpr std::uint32_t reg_ctrl = 0x00u;
    static constexpr std::uint32_t reg_mode = 0x10u;
    static constexpr std::uint32_t reg_stat = 0x20u;
    static constexpr std::uint32_t reg_tag0 = 0x40u;
    static constexpr std::uint32_t reg_cnt = 0x80u;
    static constexpr std::uint32_t reg_p3cnt = 0x90u;
    static constexpr std::uint32_t reg_p3tag = 0xA0u;

    // GIF_STAT bits.
    static constexpr std::uint32_t stat_m3r = 1u << 0;
    static constexpr std::uint32_t stat_m3p = 1u << 1;
    static constexpr std::uint32_t stat_imt = 1u << 2;
    static constexpr std::uint32_t stat_pse = 1u << 3;
    static constexpr std::uint32_t stat_p3q = 1u << 6;

    struct PathStats {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
    };

    Gif(gs::IGsSink& sink, GsPrivDevice& priv) noexcept : sink_(sink), priv_(priv) {}

    std::string_view name() const override { return "gif"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    bool read32(std::uint32_t offset, std::uint32_t& value, const MmioContext& at) override;
    bool write32(std::uint32_t offset, std::uint32_t value, std::uint32_t mask,
                 const MmioContext& at) override;
    bool write128(std::uint32_t offset, const std::uint8_t* in, const MmioContext& at) override;

    // Quadwords arriving on a path (size a multiple of 16; trailing bytes are ignored).
    void path_data(gs::GifPath path, std::span<const std::uint8_t> qwords);

    // VIF1 MSKPATH3.
    void set_vif_path3_mask(bool masked);
    // PATH3 packets waiting for the mask to lift.
    bool path3_queued() const noexcept { return !path3_queue_.empty(); }
    // Called each time the PATH3 queue drains (the DMAC completes a GIF channel transfer it held back).
    void set_path3_drained_callback(std::function<void()> callback) { path3_drained_ = std::move(callback); }

    // GS->host readback: bytes still expected from the last TRXDIR = 1, and the reader VIF1 uses.
    std::uint64_t readback_pending() const noexcept { return readback_pending_; }
    std::size_t read_back(std::span<std::uint8_t> out);

    const PathStats& stats(gs::GifPath path) const noexcept { return stats_[index(path)]; }

  private:
    struct PathState {
        std::vector<std::uint8_t> packet; // the packet being assembled, tags included
        std::uint64_t remaining = 0;      // payload quadwords left in the current tag (0: next is a tag)
        bool eop = false;                 // the current tag ends the packet
    };

    static unsigned index(gs::GifPath path) noexcept { return static_cast<unsigned>(path) - 1u; }
    bool path3_masked() const noexcept { return vif_mask_ || (mode_ & stat_m3r) != 0; }
    void packet_complete(gs::GifPath path, std::vector<std::uint8_t> packet);
    void deliver(gs::GifPath path, std::span<const std::uint8_t> packet);
    void scan(std::span<const std::uint8_t> packet);
    void ad_write(std::uint8_t reg, std::uint64_t data);
    void drain_path3();

    gs::IGsSink& sink_;
    GsPrivDevice& priv_;
    MachineServices* machine_ = nullptr;
    std::optional<RegisterWindow> regs_window_;
    std::optional<RegisterWindow> fifo_window_;

    std::array<PathState, 3> paths_{};
    std::array<PathStats, 3> stats_{};
    std::deque<std::vector<std::uint8_t>> path3_queue_;
    std::function<void()> path3_drained_;
    bool vif_mask_ = false;
    std::uint32_t ctrl_ = 0;
    std::uint32_t mode_ = 0;
    std::array<std::uint32_t, 4> last_tag_{};

    // Readback state from the scanned A+D writes.
    std::uint64_t bitbltbuf_ = 0;
    std::uint64_t trxreg_ = 0;
    std::uint64_t readback_pending_ = 0;
};

} // namespace dq8::rt
