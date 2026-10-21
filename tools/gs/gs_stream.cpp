// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "gs_stream.h"

#include <cstring>

namespace dq8::tools::gs {

namespace {

constexpr std::uint32_t fake_crc = 0xFFFFFFFFu;
constexpr std::uint32_t min_state_version = 8;
constexpr std::uint32_t max_state_version = 9;
// Version 9 appended 61 bytes of state we do not use.
constexpr std::size_t version9_extra = 61;

struct Header {
    std::uint32_t version, state_size, serial_offset, serial_size, crc, screenshot_width, screenshot_height,
        screenshot_offset, screenshot_size;
};
static_assert(sizeof(Header) == 36);

template <typename T> T load(const std::uint8_t*& p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    p += sizeof v;
    return v;
}

} // namespace

GsStream::GsStream(const std::filesystem::path& file, StreamFormat format) : file_(file) {
    in_.open(file, std::ios::binary);
    if (!in_) {
        throw StreamError("cannot open " + file.string());
    }
    std::uint32_t magic = 0;
    in_.read(reinterpret_cast<char*>(&magic), sizeof magic);
    in_.clear();
    in_.seekg(0);
    is_dump_ = format == StreamFormat::dump || (format == StreamFormat::automatic && magic == fake_crc);
    if (is_dump_) {
        read_dump_header();
    }
    first_packet_ = in_.tellg();
}

void GsStream::read_exact(void* dst, std::size_t bytes, const char* what) {
    in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(bytes));
    if (static_cast<std::size_t>(in_.gcount()) != bytes) {
        throw StreamError(file_.string() + ": truncated " + what);
    }
}

void GsStream::read_dump_header() {
    std::uint32_t crc = 0;
    std::uint32_t header_size = 0;
    read_exact(&crc, sizeof crc, "dump header");
    read_exact(&header_size, sizeof header_size, "dump header");
    if (crc != fake_crc || header_size < sizeof(Header)) {
        throw StreamError(file_.string() + " is not an uncompressed PCSX2 GS dump");
    }
    std::vector<std::uint8_t> header_bytes(header_size);
    read_exact(header_bytes.data(), header_bytes.size(), "dump header");
    Header header{};
    std::memcpy(&header, header_bytes.data(), sizeof header);
    if (header.version < min_state_version || header.version > max_state_version) {
        throw StreamError(file_.string() + ": GS dump state version " + std::to_string(header.version) +
                          " (supported: 8 and 9)");
    }
    // Offsets of the serial are relative to the header, which starts after the size field.
    if (header.serial_size != 0 &&
        std::size_t{header.serial_offset} + header.serial_size <= header_bytes.size()) {
        state_.serial.assign(reinterpret_cast<const char*>(header_bytes.data()) + header.serial_offset,
                             header.serial_size);
    }
    state_.screenshot_width = header.screenshot_width;
    state_.screenshot_height = header.screenshot_height;

    std::vector<std::uint8_t> state(header.state_size);
    read_exact(state.data(), state.size(), "GS state");
    const std::uint8_t* p = state.data();
    const std::uint8_t* const end = state.data() + state.size();
    const std::size_t regs_bytes = 4 + 15 * 8 + 2 * 12 * 8 + 2 * 8 + 2 * 4 + 8 + 2 * 4 + 2 * 4;
    const std::size_t fixed = regs_bytes + gs_vram_size + 4 * (16 + 4) + 4;
    if (state.size() < fixed) {
        throw StreamError(file_.string() + ": GS state too small (" + std::to_string(state.size()) +
                          " bytes)");
    }

    DumpState& s = state_;
    s.version = load<std::uint32_t>(p);
    for (std::uint64_t* reg :
         {&s.prim, &s.prmodecont, &s.texclut, &s.scanmsk, &s.texa, &s.fogcol, &s.dimx, &s.dthe, &s.colclamp,
          &s.pabe, &s.bitbltbuf, &s.trxdir, &s.trxpos, &s.trxreg}) {
        *reg = load<std::uint64_t>(p);
    }
    p += 8; // unused
    for (DumpState::Context& c : s.ctx) {
        for (std::uint64_t* reg : {&c.xyoffset, &c.tex0, &c.tex1, &c.clamp, &c.miptbp1, &c.miptbp2,
                                   &c.scissor, &c.alpha, &c.test, &c.fba, &c.frame, &c.zbuf}) {
            *reg = load<std::uint64_t>(p);
        }
    }
    s.rgbaq = load<std::uint64_t>(p);
    s.st = load<std::uint64_t>(p);
    s.uv = load<std::uint32_t>(p);
    s.fog = load<std::uint32_t>(p);
    p += 8;     // XYZ
    p += 2 * 4; // GIF register state
    p += 2 * 4; // transfer position
    if (s.version >= 9) {
        if (state.size() < fixed + version9_extra) {
            throw StreamError(file_.string() + ": version 9 GS state too small");
        }
        p += version9_extra;
    }
    s.vram.assign(p, p + gs_vram_size);
    p += gs_vram_size;
    for (DumpState::GifPathState& path : s.paths) {
        path.tag[0] = load<std::uint64_t>(p);
        path.tag[1] = load<std::uint64_t>(p);
        path.reg = load<std::uint32_t>(p);
    }
    s.internal_q = load<float>(p);
    if (p > end) {
        throw StreamError(file_.string() + ": GS state layout overrun");
    }
    read_exact(s.priv.data(), s.priv.size(), "privileged registers");
}

bool GsStream::next(Packet& out) {
    std::uint8_t type = 0;
    in_.read(reinterpret_cast<char*>(&type), 1);
    if (in_.gcount() != 1) {
        return false;
    }
    out.type = static_cast<PacketType>(type);
    out.data.clear();
    switch (out.type) {
    case PacketType::transfer:
        read_exact(&out.path, 1, "transfer packet");
        read_exact(&out.size, sizeof out.size, "transfer packet");
        out.data.resize(out.size);
        read_exact(out.data.data(), out.data.size(), "transfer payload");
        return true;
    case PacketType::vsync:
        read_exact(&out.field, 1, "vsync packet");
        return true;
    case PacketType::read_fifo:
        read_exact(&out.size, sizeof out.size, "read FIFO packet");
        return true;
    case PacketType::priv_regs:
        out.data.resize(priv_page_bytes);
        read_exact(out.data.data(), out.data.size(), "privileged-register packet");
        return true;
    }
    throw StreamError(file_.string() + ": unknown packet type " + std::to_string(type) + " at offset " +
                      std::to_string(static_cast<long long>(in_.tellg()) - 1));
}

void GsStream::rewind() {
    in_.clear();
    in_.seekg(first_packet_);
}

} // namespace dq8::tools::gs
