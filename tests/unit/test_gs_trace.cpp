// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The trace backend and its format (session B "Done when": the trace round-trip, and a VU1 program's
// XGKICK packet against a golden trace hash).
#include "dq8/core/hash.h"
#include "runtime/gs/gs_backends.h"
#include "runtime/gs/gs_pipeline.h"
#include "runtime/gs/gs_trace_format.h"
#include "support/ee_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace dq8;
using dq8::test::dma_tag;
using dq8::test::EeRig;
using dq8::test::gif_tag;
using dq8::test::put32;
using dq8::test::put64;
using dq8::test::vif_code;
using dq8::test::write_qword;

namespace {

// One parsed record of the trace format.
struct Transfer {
    std::uint8_t path = 0;
    std::vector<std::uint8_t> data;
};
struct Vsync {
    std::uint8_t field = 0;
};
struct ReadFifo {
    std::uint32_t qwords = 0;
};
struct Priv {
    std::vector<std::uint8_t> page;
};
using Record = std::variant<Transfer, Vsync, ReadFifo, Priv>;

std::uint32_t u32_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// Reads the stream as parallel-gs's GSDumpParser does in raw mode; fails the test on a malformed stream.
std::vector<Record> parse(const std::vector<std::uint8_t>& b) {
    std::vector<Record> out;
    std::size_t at = 0;
    while (at < b.size()) {
        const std::uint8_t type = b[at++];
        switch (type) {
        case 0: {
            REQUIRE(at + 5 <= b.size());
            Transfer t;
            t.path = b[at];
            const std::uint32_t size = u32_at(b, at + 1);
            at += 5;
            REQUIRE(at + size <= b.size());
            t.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at),
                          b.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
            out.emplace_back(std::move(t));
            break;
        }
        case 1:
            REQUIRE(at + 1 <= b.size());
            out.emplace_back(Vsync{b[at++]});
            break;
        case 2:
            REQUIRE(at + 4 <= b.size());
            out.emplace_back(ReadFifo{u32_at(b, at)});
            at += 4;
            break;
        case 3: {
            REQUIRE(at + gs::priv_page_size <= b.size());
            Priv p;
            p.page.assign(b.begin() + static_cast<std::ptrdiff_t>(at),
                          b.begin() + static_cast<std::ptrdiff_t>(at + gs::priv_page_size));
            at += gs::priv_page_size;
            out.emplace_back(std::move(p));
            break;
        }
        default:
            FAIL("unknown record type " << int{type});
        }
    }
    return out;
}

std::string hex(const core::Sha1::Digest& d) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    for (const std::uint8_t b : d) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 0xF]);
    }
    return s;
}

gs::GsPipelineConfig capture_config(std::vector<std::uint8_t>& out) {
    gs::GsPipelineConfig cfg;
    cfg.backend = gs::make_trace_gs_backend_sink(
        [&out](std::span<const std::uint8_t> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); });
    cfg.inline_mode = true;
    return cfg;
}

} // namespace

TEST_CASE("gs trace: records are PCSX2 dump packet bodies with no header", "[gs][trace]") {
    std::vector<std::uint8_t> out;
    auto backend = gs::make_trace_gs_backend_sink(
        [&](std::span<const std::uint8_t> b) { out.insert(out.end(), b.begin(), b.end()); });
    CHECK(backend->caps().name == "trace");
    CHECK_FALSE(backend->caps().produces_pixels);
    const std::vector<std::uint8_t> packet(32, 0xAB);
    backend->gif_transfer(gs::GifPath::path2, packet);
    std::vector<std::uint8_t> got(32, 0xFF);
    CHECK(backend->read_transfer(got) == 32u);
    CHECK(got == std::vector<std::uint8_t>(32, 0)); // zeros, as from the null backend
    gs::VsyncInfo info;
    info.odd_field = true;
    info.priv.write64(gs::PrivReg::csr, 0x2000);
    backend->vsync(info);

    REQUIRE(out.size() == (1 + 1 + 4 + 32) + (1 + 4) + (1 + gs::priv_page_size + 1 + 1));
    CHECK(out[0] == 0); // transfer
    CHECK(out[1] == 1); // PATH2 is PCSX2's path index 1
    CHECK(u32_at(out, 2) == 32u);
    CHECK(out[38] == 2);          // read FIFO
    CHECK(u32_at(out, 39) == 2u); // in quadwords
    CHECK(out[43] == 3);          // privileged registers, then the vsync
    CHECK(out[44 + 0x1000 + 1] == 0x20);
    CHECK(out[44 + gs::priv_page_size] == 1);
    CHECK(out[45 + gs::priv_page_size] == 1); // odd field
}

TEST_CASE("gs trace: a stream round-trips through the trace and a file", "[gs][trace]") {
    std::vector<std::uint8_t> captured;
    std::vector<Record> sent;
    {
        gs::GsPipeline pipe(capture_config(captured));
        for (std::uint64_t field = 0; field < 3; ++field) {
            for (std::uint8_t p = 1; p <= 3; ++p) {
                std::vector<std::uint8_t> packet(16u * (p + field),
                                                 static_cast<std::uint8_t>(p * 16 + field));
                pipe.gif_transfer(static_cast<gs::GifPath>(p), packet);
                sent.emplace_back(Transfer{static_cast<std::uint8_t>(p - 1), packet});
            }
            std::vector<std::uint8_t> back(48);
            pipe.read_transfer(back);
            sent.emplace_back(ReadFifo{3});
            gs::VsyncInfo info;
            info.field = field;
            info.odd_field = (field & 1) != 0;
            info.priv.write64(gs::PrivReg::dispfb1, field * 7);
            pipe.vsync(info);
            sent.emplace_back(Priv{std::vector<std::uint8_t>(info.priv.page.begin(), info.priv.page.end())});
            sent.emplace_back(Vsync{static_cast<std::uint8_t>(info.odd_field)});
        }
        CHECK(pipe.vsyncs() == 3u);
    }
    const std::vector<Record> parsed = parse(captured);
    REQUIRE(parsed.size() == sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        INFO("record " << i);
        REQUIRE(parsed[i].index() == sent[i].index());
        if (const auto* t = std::get_if<Transfer>(&parsed[i])) {
            CHECK(t->path == std::get<Transfer>(sent[i]).path);
            CHECK(t->data == std::get<Transfer>(sent[i]).data);
        } else if (const auto* v = std::get_if<Vsync>(&parsed[i])) {
            CHECK(v->field == std::get<Vsync>(sent[i]).field);
        } else if (const auto* r = std::get_if<ReadFifo>(&parsed[i])) {
            CHECK(r->qwords == std::get<ReadFifo>(sent[i]).qwords);
        } else {
            CHECK(std::get<Priv>(parsed[i]).page == std::get<Priv>(sent[i]).page);
        }
    }

    // The same stream through the file backend writes the same bytes.
    const std::filesystem::path file = std::filesystem::temp_directory_path() /
                                       ("dq8_gs_trace_test_" + std::to_string(captured.size()) + ".gs");
    {
        gs::GsPipelineConfig cfg;
        cfg.backend = gs::make_trace_gs_backend(file);
        gs::GsPipeline pipe(std::move(cfg)); // threaded this time
        for (const Record& r : sent) {
            if (const auto* t = std::get_if<Transfer>(&r)) {
                pipe.gif_transfer(static_cast<gs::GifPath>(t->path + 1), t->data);
            } else if (const auto* f = std::get_if<ReadFifo>(&r)) {
                std::vector<std::uint8_t> back(f->qwords * 16u);
                pipe.read_transfer(back);
            } else if (const auto* v = std::get_if<Vsync>(&r)) {
                gs::VsyncInfo info;
                info.odd_field = v->field != 0;
                const auto& page = std::get<Priv>(*(&r - 1)).page;
                std::memcpy(info.priv.page.data(), page.data(), page.size());
                pipe.vsync(info);
            }
        }
    }
    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> from_file((std::istreambuf_iterator<char>(in)),
                                              std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(file);
    CHECK(from_file == captured);
    CHECK_THROWS(gs::make_trace_gs_backend("/nonexistent-dir-dq8/trace.gs"));
}

TEST_CASE("gs trace: MPG + MSCAL of a VU1 program XGKICKs a packet matching the golden trace hash",
          "[gs][trace][vu1]") {
    std::vector<std::uint8_t> trace;
    gs::GsPipeline pipe(capture_config(trace));
    EeRig rig(pipe);

    // The GIF packet the VU1 program kicks: PACKED, two A+D writes (PRIM = sprite, RGBAQ).
    std::vector<std::uint8_t> gif;
    put64(gif, gif_tag(2, 0, 1, true));
    put64(gif, 0xE);
    put64(gif, 6);
    put64(gif, 0x00);
    put64(gif, 0x3F80000080FF4020ull);
    put64(gif, 0x01);

    // VIF1 data: MPG of two instruction pairs (XGKICK vi0 with the E bit; a NOP pair for the delay slot),
    // UNPACK V4-32 of the packet to VU1 address 0, MSCAL 0.
    const std::uint32_t xgkick_vi0 = (0x40u << 25) | (0x6Cu & 0x7Cu) << 4 | (0x6Cu & 3u) | 0x3Cu;
    const std::uint32_t upper_nop = 0x000002FFu;
    std::vector<std::uint8_t> vif;
    put32(vif, vif_code(0x00, 0, 0)); // NOP: MPG's data starts on a 64-bit boundary
    put32(vif, vif_code(0x4A, 2, 0));
    put32(vif, xgkick_vi0);
    put32(vif, upper_nop | (1u << 30));
    put32(vif, 0);
    put32(vif, upper_nop);
    put32(vif, vif_code(0x6C, 3, 0));
    vif.insert(vif.end(), gif.begin(), gif.end());
    put32(vif, vif_code(0x14, 0, 0));
    while (vif.size() % 16 != 0) {
        put32(vif, 0);
    }

    // As a DMA chain: one end tag with the VIF data after it, sent through VIF1's channel.
    std::uint8_t* ram = rig.mem().rdram();
    write_qword(ram + 0x80000, dma_tag(static_cast<std::uint32_t>(vif.size() / 16), 7));
    std::memcpy(ram + 0x80010, vif.data(), vif.size());
    rig.w32(0x10009030u, 0x80000);
    rig.w32(0x10009000u, 0x105u);
    rig.machine.fire_vblank_start();

    // Independent checks before the hash: one PATH1 transfer carrying exactly the packet, then the vsync.
    const std::vector<Record> parsed = parse(trace);
    REQUIRE(parsed.size() == 3u);
    const auto& t = std::get<Transfer>(parsed[0]);
    CHECK(t.path == 0);
    CHECK(t.data == gif);
    CHECK(std::holds_alternative<Priv>(parsed[1]));
    CHECK(std::holds_alternative<Vsync>(parsed[2]));
    CHECK(rig.vu().stats(1).xgkicks == 1u);

    const std::string trace_hash = hex(core::Sha1::of(trace));
    CHECK(trace_hash == "cbd3eb3bcc941fcef59864047d279c7a49b63c72");
    // One field: the stream hash is SHA-1 over h_0 (20 zero bytes) and that field's trace bytes.
    core::Sha1 chain;
    chain.update(std::vector<std::uint8_t>(20, 0));
    chain.update(trace);
    CHECK(pipe.stream_hash() == hex(chain.finish()));
    CHECK(pipe.stream_hash() == "c796bc7ebc0ef9a27188c7ebcd3fb555dfd34712");
}
