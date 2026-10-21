// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Reader for GS packet streams (tools/gs/README.md):
// - a PCSX2 GS dump, uncompressed, state version 8 or 9: a header, the GS state the capture started from, the
//   8 KiB privileged-register page, then packets;
// - a raw trace: packets only, no header (our trace backend, `parallel-gs-replayer --raw`).
// Packets: 0 = transfer {u8 path, u32 size, size bytes}, 1 = vsync {u8 field}, 2 = read FIFO {u32 size},
// 3 = privileged registers {8,192 bytes}. All little-endian.
namespace dq8::tools::gs {

class StreamError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class StreamFormat { automatic, dump, raw };

enum class PacketType : std::uint8_t { transfer = 0, vsync = 1, read_fifo = 2, priv_regs = 3 };

struct Packet {
    PacketType type = PacketType::vsync;
    std::uint8_t path = 0;          // transfer: the dump's path index, 0..3 (3 = PCSX2's merged GIF stream)
    std::uint8_t field = 0;         // vsync
    std::uint32_t size = 0;         // transfer and read FIFO: bytes
    std::vector<std::uint8_t> data; // transfer payload or the privileged page
};

inline constexpr std::size_t gs_vram_size = 4u << 20;
inline constexpr std::size_t priv_page_bytes = 0x2000;

// The GS state a PCSX2 dump starts from (GSdx freeze layout, state versions 8 and 9).
struct DumpState {
    std::uint32_t version = 0;
    std::string serial;
    std::uint32_t screenshot_width = 0;
    std::uint32_t screenshot_height = 0;

    // General registers, in the order the state stores them.
    std::uint64_t prim = 0, prmodecont = 0, texclut = 0, scanmsk = 0, texa = 0, fogcol = 0, dimx = 0,
                  dthe = 0, colclamp = 0, pabe = 0, bitbltbuf = 0, trxdir = 0, trxpos = 0, trxreg = 0;
    struct Context {
        std::uint64_t xyoffset = 0, tex0 = 0, tex1 = 0, clamp = 0, miptbp1 = 0, miptbp2 = 0, scissor = 0,
                      alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
    };
    std::array<Context, 2> ctx{};
    std::uint64_t rgbaq = 0, st = 0;
    std::uint32_t uv = 0, fog = 0;

    std::vector<std::uint8_t> vram; // 4 MiB in GS memory order
    struct GifPathState {
        std::array<std::uint64_t, 2> tag{};
        std::uint32_t reg = 0;
        std::uint32_t nloop() const noexcept { return static_cast<std::uint32_t>(tag[0] & 0x7FFF); }
    };
    std::array<GifPathState, 4> paths{};
    float internal_q = 1.0f;
    std::array<std::uint8_t, priv_page_bytes> priv{};
};

class GsStream {
  public:
    // Throws StreamError when the file cannot be read or is not a stream of the given format.
    GsStream(const std::filesystem::path& file, StreamFormat format);

    bool is_dump() const noexcept { return is_dump_; }
    // Only for dumps.
    const DumpState& state() const noexcept { return state_; }

    // The next packet; false at the end of the stream. Throws StreamError on a truncated or unknown packet.
    bool next(Packet& out);
    // Back to the first packet.
    void rewind();

  private:
    void read_exact(void* dst, std::size_t bytes, const char* what);
    void read_dump_header();

    std::filesystem::path file_;
    std::ifstream in_;
    bool is_dump_ = false;
    std::streamoff first_packet_ = 0;
    DumpState state_;
};

} // namespace dq8::tools::gs
