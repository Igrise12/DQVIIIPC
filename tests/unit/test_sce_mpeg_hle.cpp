// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The sceMpeg HLE (runtime/movie/sce_mpeg_hle.h) driving a fake movie player on a FakeMachine: a decoder loop
// calls IsEnd and GetPicture into two slots, and the player's nodata callback (an injected guest call) runs a
// reader that writes PES payloads into a wrapping demux ring and offers them to the replaced video stream
// callback, keeping a refused packet for later as sceMpegDemuxPssRing does. Streams come from the test-time
// MPEG-2 writer; the dq8-movie case SKIPs when the library is not built.
#include "dq8/core/hash.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/ee/overrides.h"
#include "runtime/movie/ipu_regs.h"
#include "runtime/movie/macroblock_writer.h"
#include "runtime/movie/movie_decoder.h"
#include "runtime/movie/mpeg_es_model.h"
#include "runtime/movie/sce_mpeg_hle.h"
#include "support/fake_machine.h"
#include "support/mpeg2_writer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dq8::rt;
using namespace dq8::test;

namespace {

constexpr std::uint32_t k_mp = 0x00200000u;
constexpr std::uint32_t k_name = 0x00200400u;
constexpr std::uint32_t k_cbdata = 0x00200800u;
constexpr std::uint32_t k_ring = 0x00300000u;
constexpr std::uint32_t k_ring_bytes = 4000;                     // small and odd, so payloads wrap
constexpr std::uint32_t k_slots[2] = {0x20400000u, 0x20500000u}; // uncached, as the player passes them
constexpr std::uint32_t k_nodata = 0x001A0000u;
constexpr std::uint32_t k_video_fn = 0x001A1000u;
constexpr std::uint32_t k_pcm_fn = 0x001A2000u;
constexpr std::uint32_t k_get_picture = 0x00111000u;
constexpr std::uint32_t k_return = 0x00123450u;
constexpr std::uint32_t k_sp = 0x01F00000u;
constexpr std::uint32_t k_width = 64;
constexpr std::uint32_t k_height = 48;
constexpr std::uint32_t k_macroblocks = (k_width / 16) * (k_height / 16);

class FakeGuest final : public IGuestCalls {
  public:
    std::function<std::uint64_t(std::uint32_t entry, std::span<const std::uint64_t> args)> on_call;
    std::uint64_t call_function(R5900Context& /*ctx*/, std::uint32_t entry,
                                std::span<const std::uint64_t> args, std::uint32_t /*sp*/) override {
        return on_call(entry, args);
    }
    std::uint64_t call_kernel(R5900Context&, std::string_view, std::span<const std::uint64_t>) override {
        throw std::logic_error("the movie HLE makes no kernel calls");
    }
};

// A decoder that paints each picture a flat luma from its tag, in the reference order of a real decoder.
class FakeDecoder final : public IMovieDecoder {
  public:
    std::string name() const override { return "fake"; }
    void reset() override { order_ = make_blank_movie_decoder(); }
    void decode_picture(std::span<const std::uint8_t> coded, std::int64_t tag) override {
        order_->decode_picture(coded, tag);
    }
    void flush() override { order_->flush(); }
    bool next_picture(PictureYuv& out) override {
        if (!order_->next_picture(out)) {
            return false;
        }
        y_.assign(std::size_t{out.width} * out.height, static_cast<std::uint8_t>(40 + out.pts90k * 7));
        out.y = y_.data();
        out.y_stride = out.width;
        return true;
    }

  private:
    std::unique_ptr<IMovieDecoder> order_ = make_blank_movie_decoder();
    std::vector<std::uint8_t> y_;
};

struct Packet {
    std::vector<std::uint8_t> payload;
    std::int64_t pts = -1;
    std::int64_t dts = -1;
};

std::vector<Packet> packetise(std::span<const std::uint8_t> es, std::size_t size) {
    std::vector<Packet> out;
    const std::vector<std::size_t> starts = mpeg2_picture_offsets(es);
    std::size_t next_start = 0;
    for (std::size_t at = 0; at < es.size(); at += size) {
        Packet p;
        const std::size_t n = std::min(size, es.size() - at);
        p.payload.assign(es.begin() + static_cast<std::ptrdiff_t>(at),
                         es.begin() + static_cast<std::ptrdiff_t>(at + n));
        while (next_start < starts.size() && starts[next_start] < at) {
            ++next_start;
        }
        if (next_start < starts.size() && starts[next_start] < at + n) {
            p.pts = static_cast<std::int64_t>(3000 * (next_start + 1)); // a PTS where a picture starts
            p.dts = p.pts - 3000;
        }
        out.push_back(std::move(p));
    }
    return out;
}

struct Picture {
    std::uint32_t frame = 0;
    std::int64_t pts = 0;
    std::vector<std::uint8_t> rgb32;
};

struct Run {
    std::vector<MachineEvent> events;
    std::vector<Picture> pictures;
    SceMpegHle::Stats stats{};
    Cycles cycles = 0;
    std::uint32_t refused_packets = 0;
};

struct PlayerOptions {
    std::function<std::unique_ptr<IMovieDecoder>()> make_decoder = [] { return make_blank_movie_decoder(); };
    std::size_t packet_size = 700;
    std::uint32_t packets_per_nodata = 64; // the reader demuxes until refused or this many
    std::uint32_t setth = 0;               // a SETTH value for the IPU stub (0: none)
};

class Player {
  public:
    explicit Player(const PlayerOptions& o, std::vector<Packet> packets)
        : options_(o), packets_(packets.begin(), packets.end()) {
        SceMpegHleConfig config;
        config.make_decoder = o.make_decoder;
        config.video_callback = k_video_fn;
        hle_ = std::make_unique<SceMpegHle>(config);
        m_.attach(ipu_);
        m_.attach(*hle_);
        if (o.setth != 0) {
            std::uint8_t bytes[4];
            std::memcpy(bytes, &o.setth, 4);
            REQUIRE(m_.mmio().write(IpuRegisters::regs_base, 4, bytes, MmioContext{}));
        }
        guest_.on_call = [this](std::uint32_t entry, std::span<const std::uint64_t> args) {
            REQUIRE(entry == k_nodata);
            REQUIRE(args.size() == 3);
            CHECK(args[0] == k_mp);
            CHECK(m_.memory().load32(static_cast<std::uint32_t>(args[1])) == SceMpegHle::callback_nodata);
            CHECK(args[2] == 0x55);
            ++nodata_calls_;
            reader();
            return nodata_result_;
        };
    }

    // A call of the guest function at `address` with these arguments, on a context of its own (calls nest:
    // the reader runs inside GetPicture's nodata callback).
    OverrideCall call(std::uint32_t address, std::initializer_list<std::uint32_t> args) {
        R5900Context& ctx = contexts_.emplace_back();
        unsigned i = 0;
        for (const std::uint32_t a : args) {
            set_gpr_lo(ctx, i < 4 ? 4 + i : 8 + (i - 4), a);
            ++i;
        }
        set_gpr_lo(ctx, 29, k_sp);
        set_gpr_lo(ctx, 31, k_return);
        ctx.pc = address;
        return OverrideCall{m_.memory().rdram(), &ctx, nullptr, &m_, nullptr, address, &guest_, 0};
    }
    static std::int64_t result(const OverrideCall& c) {
        return static_cast<std::int32_t>(gpr_u32(*c.ctx, 2));
    }

    void open(const std::string& name) {
        std::vector<std::uint8_t> bytes(name.begin(), name.end());
        bytes.push_back(0);
        m_.memory().write_bytes(k_name, bytes);
        auto c = call(0x001A3000u, {0x1000, 0x2000, k_name});
        hle_->hook_player_open(c, 2);
        auto create = call(0x00110000u, {k_mp, 0x00600000, 0x1C000});
        hle_->hook_create(create);
        for (const auto& [type, fn] :
             {std::pair{0u, 0x001A4000u}, {1u, k_nodata}, {2u, 0x001A5000u}, {5u, 0x001A6000u}}) {
            auto add = call(0x00111540u, {k_mp, type, fn, type == 1 ? 0x55u : 0u});
            hle_->hook_add_callback(add);
        }
        auto video = call(0x001159C0u, {k_mp, SceMpegHle::stream_video, 0, k_video_fn, k_ring});
        hle_->hook_add_str_callback(video);
        auto pcm = call(0x001159C0u, {k_mp, 2, 0, k_pcm_fn, k_ring});
        hle_->hook_add_str_callback(pcm);
    }

    // The reader thread: DemuxPssRing over what it has, the video callback refusing when the HLE is full.
    void reader() {
        auto demux = call(0x001155C0u, {k_mp, k_ring, 0, k_ring, k_ring_bytes});
        hle_->hook_demux_pss_ring(demux);
        for (std::uint32_t n = 0; n < options_.packets_per_nodata && !packets_.empty(); ++n) {
            const Packet& p = packets_.front();
            // The payload where the ring's write position is, wrapping at the ring's end.
            const std::uint32_t at = k_ring + write_ % k_ring_bytes;
            const std::uint32_t first = std::min<std::uint32_t>(static_cast<std::uint32_t>(p.payload.size()),
                                                                k_ring + k_ring_bytes - at);
            m_.memory().write_bytes(at, std::span<const std::uint8_t>(p.payload).first(first));
            m_.memory().write_bytes(k_ring, std::span<const std::uint8_t>(p.payload).subspan(first));
            m_.memory().fill(k_cbdata, 0x20, 0);
            m_.memory().store32(k_cbdata + 0x00, 6);
            m_.memory().store32(k_cbdata + 0x08, at);
            m_.memory().store32(k_cbdata + 0x0C, static_cast<std::uint32_t>(p.payload.size()));
            m_.memory().store64(k_cbdata + 0x10, static_cast<std::uint64_t>(p.pts));
            m_.memory().store64(k_cbdata + 0x18, static_cast<std::uint64_t>(p.dts));
            // The SDK's video callback (replaced by the HLE): (mp, callback data, the registered data word).
            auto video = call(k_video_fn, {k_mp, k_cbdata, k_ring});
            hle_->video_stream(video);
            CHECK(video.ctx->pc == k_return);
            if (result(video) == 0) {
                ++refused_; // DemuxPssRing stops; the packet stays in the ring
                return;
            }
            write_ += static_cast<std::uint32_t>(p.payload.size());
            packets_.pop_front();
        }
    }

    // The decoder thread: while !IsEnd, GetPicture into the next slot.
    Run play() {
        Run r;
        open("TEST_W.PSS");
        for (std::uint32_t slot = 0;; slot ^= 1u) {
            auto end = call(0x001114E8u, {k_mp});
            hle_->is_end(end);
            if (result(end) != 0) {
                break;
            }
            auto get = call(k_get_picture, {k_mp, k_slots[slot], k_macroblocks});
            hle_->get_picture(get);
            REQUIRE(get.ctx->pc == k_return);
            REQUIRE(result(get) == 0);
            CHECK(m_.memory().load32(k_mp + 0) == k_width);
            CHECK(m_.memory().load32(k_mp + 4) == k_height);
            Picture p;
            p.frame = m_.memory().load32(k_mp + 8);
            p.pts = static_cast<std::int64_t>(m_.memory().load64(k_mp + 0x10));
            p.rgb32.resize(std::size_t{k_macroblocks} * macroblock_rgb32_bytes);
            m_.memory().read_bytes(k_slots[slot], p.rgb32);
            r.pictures.push_back(std::move(p));
            REQUIRE(r.pictures.size() < 1000);
        }
        auto reset = call(0x00111468u, {k_mp});
        hle_->hook_reset(reset);
        auto del = call(0x00111380u, {k_mp});
        hle_->hook_delete(del);
        r.events = m_.emitted;
        r.stats = hle_->stats();
        r.cycles = m_.clock().now();
        r.refused_packets = refused_;
        return r;
    }

    FakeMachine& machine() { return m_; }
    SceMpegHle& hle() { return *hle_; }
    std::uint32_t nodata_calls() const { return nodata_calls_; }
    std::uint64_t nodata_result_ = 1;

  private:
    PlayerOptions options_;
    std::deque<Packet> packets_;
    FakeMachine m_;
    IpuRegisters ipu_;
    std::unique_ptr<SceMpegHle> hle_;
    FakeGuest guest_;
    std::deque<R5900Context> contexts_;
    std::uint32_t write_ = 0;
    std::uint32_t refused_ = 0;
    std::uint32_t nodata_calls_ = 0;
};

struct TestMovie {
    Mpeg2TestStream spec;
    std::vector<std::uint32_t> display_seeds;
    std::vector<std::uint8_t> es;
};

TestMovie test_movie(std::uint32_t count = 20, std::uint32_t b_frames = 2) {
    TestMovie t;
    t.spec.width = k_width;
    t.spec.height = k_height;
    t.spec.pictures = mpeg2_test_gop(count, b_frames, t.display_seeds);
    for (Mpeg2TestPicture& p : t.spec.pictures) {
        p.cb = 128;
        p.cr = 128;
    }
    t.es = write_mpeg2_stream(t.spec);
    return t;
}

std::string field(const MachineEvent& e, const std::string& key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            if (const auto* s = std::get_if<std::string>(&v)) {
                return *s;
            }
            if (const auto* u = std::get_if<std::uint64_t>(&v)) {
                return std::to_string(*u);
            }
        }
    }
    return "?";
}

std::uint8_t grey(int luma) {
    return static_cast<std::uint8_t>(((0x95 * (luma - 16) >> 6) + 1) >> 1);
}

} // namespace

TEST_CASE("sceMpeg HLE: nodecode plays a fake player's movie to the end", "[movie]") {
    const TestMovie t = test_movie();
    PlayerOptions o;
    Player player(o, packetise(t.es, o.packet_size));
    const Run r = player.play();

    REQUIRE(r.pictures.size() == 20);
    for (std::uint32_t i = 0; i < r.pictures.size(); ++i) {
        CHECK(r.pictures[i].frame == i);
        // Black, opaque: no SETTH.
        CHECK(r.pictures[i].rgb32[0] == 0);
        CHECK(r.pictures[i].rgb32[3] == 0x80);
    }
    CHECK(r.pictures[0].pts == 3000); // the I picture's PES timestamp
    CHECK(r.cycles == 20 * player.machine().cost().movie_picture);
    CHECK(r.stats.pictures == 20);
    CHECK(r.stats.decoder_errors == 0);
    CHECK(r.stats.mismatched == 0);
    CHECK(r.stats.refusals == r.refused_packets);
    CHECK(r.refused_packets > 0); // the reader offers everything; the HLE holds at most four pictures
    CHECK(player.hle().open_sessions() == 0);

    REQUIRE(r.events.size() == 3);
    CHECK(r.events[0].type == "movie");
    CHECK(field(r.events[0], "phase") == "open");
    CHECK(field(r.events[0], "name") == "TEST_W.PSS");
    CHECK(field(r.events[0], "pictures") == "0");
    CHECK(field(r.events[1], "phase") == "end");
    CHECK(field(r.events[1], "pictures") == "20");
    CHECK(field(r.events[1], "slot_a") == "0x00400000");
    CHECK(field(r.events[1], "slot_b") == "0x00500000");
    CHECK(field(r.events[2], "phase") == "delete");
    CHECK(field(r.events[2], "name") == "TEST_W.PSS");
}

TEST_CASE("sceMpeg HLE: backpressure works however the reader feeds", "[movie]") {
    const TestMovie t = test_movie(14, 3);
    for (const std::size_t packet : {std::size_t{97}, std::size_t{700}, std::size_t{4096}}) {
        for (const std::uint32_t per_call : {1u, 3u, 1000u}) {
            PlayerOptions o;
            o.packet_size = packet;
            o.packets_per_nodata = per_call;
            Player player(o, packetise(t.es, packet));
            const Run r = player.play();
            INFO("packet " << packet << ", packets per nodata call " << per_call);
            CHECK(r.pictures.size() == 14);
            CHECK(r.stats.pictures == 14);
            CHECK(player.nodata_calls() > 0);
        }
    }
}

TEST_CASE("sceMpeg HLE: nodecode and a painting decoder give identical events", "[movie]") {
    const TestMovie t = test_movie();
    PlayerOptions blank;
    PlayerOptions fake;
    fake.make_decoder = [] { return std::make_unique<FakeDecoder>(); };
    Player a(blank, packetise(t.es, 700));
    Player b(fake, packetise(t.es, 700));
    const Run ra = a.play();
    const Run rb = b.play();
    REQUIRE(ra.events.size() == rb.events.size());
    for (std::size_t i = 0; i < ra.events.size(); ++i) {
        CHECK(ra.events[i].type == rb.events[i].type);
        CHECK(ra.events[i].fields == rb.events[i].fields);
    }
    CHECK(ra.cycles == rb.cycles);
    REQUIRE(ra.pictures.size() == rb.pictures.size());
    // Only the decode slots differ: the painting decoder's pictures carry their tags in display order.
    const std::vector<std::uint32_t>& seeds = t.display_seeds;
    for (std::size_t k = 0; k < rb.pictures.size(); ++k) {
        std::size_t tag = 0;
        while (t.spec.pictures[tag].seed != seeds[k]) {
            ++tag;
        }
        CHECK(rb.pictures[k].rgb32[0] == grey(static_cast<int>(40 + tag * 7)));
        CHECK(ra.pictures[k].rgb32[0] == 0);
    }
}

TEST_CASE("sceMpeg HLE: dq8-movie paints the writer's pictures through the IPU CSC", "[movie][dq8_movie]") {
    if (movie_library_problem(MovieLibraryConfig{})) {
        SKIP("dq8-movie is not available to the default search in this build");
    }
    const TestMovie t = test_movie(13, 2);
    PlayerOptions o;
    o.make_decoder = [] { return make_library_movie_decoder(MovieLibraryConfig{}); };
    Player player(o, packetise(t.es, 900));
    const Run r = player.play();
    REQUIRE(r.pictures.size() == 13);
    CHECK(r.stats.decoder_errors == 0);
    CHECK(r.stats.mismatched == 0);
    for (std::size_t k = 0; k < r.pictures.size(); ++k) {
        // Tile n is macroblock (n / 3, n % 3): column-major over 4 columns of 3.
        for (std::uint32_t n = 0; n < k_macroblocks; ++n) {
            const std::uint32_t mbx = n / 3;
            const std::uint32_t mby = n % 3;
            INFO("picture " << k << " tile " << n);
            const std::uint8_t* px =
                r.pictures[k].rgb32.data() + std::size_t{n} * macroblock_rgb32_bytes + 5 * 64 + 6 * 4;
            const std::uint8_t g = grey(mpeg2_test_luma(t.display_seeds[k], mbx, mby));
            CHECK(px[0] == g);
            CHECK(px[1] == g);
            CHECK(px[2] == g);
            CHECK(px[3] == 0x80);
        }
    }
}

TEST_CASE("sceMpeg HLE: SETTH thresholds reach the pictures", "[movie]") {
    const TestMovie t = test_movie(4, 2);
    PlayerOptions o;
    o.setth = (0x9u << 28) | (0x40u << 16) | 0x10u; // black is below th0: transparent
    Player player(o, packetise(t.es, 700));
    const Run r = player.play();
    REQUIRE(r.pictures.size() == 4);
    CHECK(r.pictures[0].rgb32[3] == 0);
}

TEST_CASE("sceMpeg HLE: the nodata callback can make GetPicture give up", "[movie]") {
    const TestMovie t = test_movie(4, 2);
    PlayerOptions o;
    Player player(o, packetise(t.es, 700));
    player.nodata_result_ = 0;
    player.open("X");
    auto get = player.call(k_get_picture, {k_mp, k_slots[0], k_macroblocks});
    player.hle().get_picture(get);
    CHECK(Player::result(get) == -1);
    CHECK(player.nodata_calls() == 1);
}

TEST_CASE("sceMpeg HLE: paths it does not cover stop the machine", "[movie]") {
    const TestMovie t = test_movie(4, 2);
    PlayerOptions o;
    Player player(o, packetise(t.es, 700));
    SECTION("a call on an mp no Create set up") {
        auto get = player.call(k_get_picture, {0x00700000u, k_slots[0], k_macroblocks});
        CHECK_THROWS_AS(player.hle().get_picture(get), GuestStop);
        auto end = player.call(0x001114E8u, {0x00700000u});
        CHECK_THROWS_AS(player.hle().is_end(end), GuestStop);
    }
    SECTION("the SDK's own IPU decode") {
        auto inner = player.call(0x00110B28u, {k_mp});
        try {
            player.hle().decode_reached(inner);
            FAIL("no stop");
        } catch (const GuestStop& e) {
            CHECK(e.info().message.find("IPU picture decode") != std::string::npos);
        }
    }
    SECTION("video sent to another callback") {
        auto create = player.call(0x00110000u, {k_mp, 0x00600000, 0x1C000});
        player.hle().hook_create(create);
        auto other = player.call(0x001159C0u, {k_mp, SceMpegHle::stream_video, 0, 0x001A9000u, k_ring});
        CHECK_THROWS_AS(player.hle().hook_add_str_callback(other), GuestStop);
    }
    SECTION("no picture and no nodata callback") {
        auto create = player.call(0x00110000u, {k_mp, 0x00600000, 0x1C000});
        player.hle().hook_create(create);
        auto get = player.call(k_get_picture, {k_mp, k_slots[0], k_macroblocks});
        CHECK_THROWS_AS(player.hle().get_picture(get), GuestStop);
    }
    SECTION("a video stream the model refuses") {
        player.open("X");
        std::vector<std::uint8_t> field_picture = t.es;
        const std::vector<std::size_t> starts = mpeg2_picture_offsets(field_picture);
        std::size_t ext = starts[0];
        while (!(field_picture[ext] == 0 && field_picture[ext + 1] == 0 && field_picture[ext + 2] == 1 &&
                 field_picture[ext + 3] == 0xB5)) {
            ++ext;
        }
        field_picture[ext + 6] = static_cast<std::uint8_t>((field_picture[ext + 6] & ~3u) | 2u);
        const auto length =
            static_cast<std::uint32_t>(std::min<std::size_t>(field_picture.size(), k_ring_bytes));
        player.machine().memory().write_bytes(k_ring,
                                              std::span<const std::uint8_t>(field_picture).first(length));
        player.machine().memory().fill(k_cbdata, 0x20, 0);
        player.machine().memory().store32(k_cbdata + 0x08, k_ring);
        player.machine().memory().store32(k_cbdata + 0x0C, length);
        auto video = player.call(k_video_fn, {k_mp, k_cbdata, k_ring});
        CHECK_THROWS_AS(player.hle().video_stream(video), GuestStop);
    }
}

TEST_CASE("sceMpeg HLE: a configuration that could never deliver is refused", "[movie]") {
    SceMpegHleConfig c;
    CHECK_THROWS_AS(SceMpegHle(c), std::invalid_argument);
    c.make_decoder = [] { return make_blank_movie_decoder(); };
    c.max_pending_pictures = 1;
    CHECK_THROWS_AS(SceMpegHle(c), std::invalid_argument);
}
