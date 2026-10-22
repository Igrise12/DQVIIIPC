// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The sceMpeg HLE's elementary-stream model (runtime/movie/mpeg_es_model.h) on streams from the test-time
// MPEG-2 writer: picture split, display order, timestamps, end of stream, backpressure count and refused
// streams.
#include "runtime/movie/mpeg_es_model.h"
#include "support/mpeg2_writer.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace dq8::rt;
using namespace dq8::test;

namespace {

struct Fed {
    std::vector<std::uint8_t> es;
    std::vector<std::uint32_t> display_seeds;
    std::vector<Mpeg2TestPicture> pictures; // decode order
};

Fed gop_stream(std::uint32_t count, std::uint32_t b_frames, bool sequence_end = true,
               bool low_delay = false) {
    Fed f;
    Mpeg2TestStream s;
    s.width = 48;
    s.height = 32;
    s.low_delay = low_delay;
    s.sequence_end = sequence_end;
    s.pictures = mpeg2_test_gop(count, b_frames, f.display_seeds);
    f.pictures = s.pictures;
    f.es = write_mpeg2_stream(s);
    return f;
}

// Feeds `es` in chunks of the given sizes (cycled), as PES payloads without timestamps.
void feed_chunks(MpegEsModel& m, std::span<const std::uint8_t> es, std::span<const std::size_t> sizes) {
    std::size_t at = 0;
    for (std::size_t i = 0; at < es.size(); ++i) {
        const std::size_t n = std::min(sizes[i % sizes.size()], es.size() - at);
        m.feed(es.subspan(at, n));
        at += n;
    }
}

// Drains the display queue: the seeds of the delivered pictures, mapped through decode order.
std::vector<std::uint32_t> drain_display(MpegEsModel& m, const std::vector<Mpeg2TestPicture>& decode_order) {
    std::vector<std::uint32_t> seeds;
    while (const MpegDisplayPicture* d = m.next_display()) {
        seeds.push_back(decode_order.at(d->index).seed);
        m.pop_display();
    }
    return seeds;
}

} // namespace

TEST_CASE("movie ES model: pictures split and reordered for display", "[movie]") {
    const Fed f = gop_stream(13, 2);
    const std::size_t sizes[] = {1, 7, 2048, 3, 500, 4096};
    MpegEsModel m;
    feed_chunks(m, f.es, sizes);
    REQUIRE(m.error().empty());
    CHECK(m.ended());
    CHECK(m.sequence().known);
    CHECK(m.sequence().width == 48);
    CHECK(m.sequence().height == 32);
    CHECK(m.sequence().frame_rate_code == 5);
    CHECK(m.sequence().mpeg2);
    CHECK_FALSE(m.sequence().low_delay);
    CHECK(m.completed() == 13);
    CHECK(m.coded_waiting() == 13);

    // Coded pictures in decode order, with the right types and exactly the bytes between start codes.
    const std::vector<std::size_t> starts = mpeg2_picture_offsets(f.es);
    REQUIRE(starts.size() == 13);
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < 13; ++i) {
        const auto pic = m.take_coded();
        REQUIRE(pic);
        CHECK(pic->index == i);
        CHECK(static_cast<char>("?IPB"[static_cast<int>(pic->type)]) == f.pictures[i].type);
        CHECK(mpeg_picture_type(pic->bytes) == pic->type);
        total += pic->bytes.size();
    }
    CHECK_FALSE(m.take_coded());
    CHECK(total + 4 == f.es.size()); // everything but the sequence end code

    CHECK(drain_display(m, f.pictures) == f.display_seeds);
    CHECK(m.finished());
    CHECK(m.delivered() == 13);
    CHECK(m.pending() == 0);
}

TEST_CASE("movie ES model: a picture is complete only when the next one begins", "[movie]") {
    const Fed f = gop_stream(4, 2); // I0 P3 B1 B2
    const std::vector<std::size_t> starts = mpeg2_picture_offsets(f.es);
    MpegEsModel m;
    const std::span<const std::uint8_t> es(f.es);

    m.feed(es.first(starts[1] + 3)); // up to the middle of P3's start code
    CHECK(m.completed() == 0);
    m.feed(es.subspan(starts[1] + 3, 3));
    CHECK(m.completed() == 1);                                // I0
    CHECK(m.next_display() == nullptr);                       // a reference waits for the next reference
    m.feed(es.subspan(starts[1] + 6, starts[2] - starts[1])); // P3 complete
    CHECK(m.completed() == 2);
    REQUIRE(m.next_display() != nullptr);
    CHECK(m.next_display()->index == 0); // I0 released by P3
    m.feed(es.subspan(starts[2] + 6, starts[3] - starts[2]));
    CHECK(m.completed() == 3); // B1 complete
    CHECK(m.pending() == 3);
    CHECK(drain_display(m, f.pictures) == std::vector<std::uint32_t>{1, 2});
    CHECK_FALSE(m.ended());
    m.feed(es.subspan(starts[3] + 6));
    CHECK(m.ended());
    CHECK(drain_display(m, f.pictures) == std::vector<std::uint32_t>{3, 4}); // B2, then P3 at the end
    CHECK(m.finished());
}

TEST_CASE("movie ES model: low delay shows every picture as soon as it is complete", "[movie]") {
    const Fed f = gop_stream(5, 0, true, true); // I P P P P
    MpegEsModel m;
    const std::vector<std::size_t> starts = mpeg2_picture_offsets(f.es);
    m.feed(std::span<const std::uint8_t>(f.es).first(starts[2] + 6));
    CHECK(m.sequence().low_delay);
    CHECK(drain_display(m, f.pictures) == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("movie ES model: end_of_stream completes a stream without a sequence end code", "[movie]") {
    const Fed f = gop_stream(7, 2, false);
    MpegEsModel m;
    m.feed(f.es);
    CHECK_FALSE(m.ended());
    CHECK(m.completed() == 6);
    m.end_of_stream();
    CHECK(m.ended());
    CHECK(m.completed() == 7);
    CHECK(drain_display(m, f.pictures) == f.display_seeds);
    CHECK(m.finished());
}

TEST_CASE("movie ES model: a picture takes the timestamps of the PES packet it starts in", "[movie]") {
    const Fed f = gop_stream(4, 2); // I0 P3 B1 B2
    const std::vector<std::size_t> starts = mpeg2_picture_offsets(f.es);
    const std::span<const std::uint8_t> es(f.es);
    MpegEsModel m;
    // Packet 1: the headers and I0 (PTS 3000, DTS 0); packet 2: P3 without timestamps; packet 3: from the
    // middle of P3 to B2's start with PTS 9000 (B1 starts in it); packet 4: the rest with PTS 12000.
    m.feed(es.first(starts[1]), 3000, 0);
    m.feed(es.subspan(starts[1], 10));
    m.feed(es.subspan(starts[1] + 10, starts[3] - starts[1] - 10), 9000, -1);
    m.feed(es.subspan(starts[3]), 12000, -1);
    std::vector<std::int64_t> pts;
    std::vector<std::int64_t> dts;
    while (auto pic = m.take_coded()) {
        pts.push_back(pic->pts90k);
        dts.push_back(pic->dts90k);
    }
    CHECK(pts == std::vector<std::int64_t>{3000, -1, 9000, 12000});
    CHECK(dts == std::vector<std::int64_t>{0, -1, -1, -1});
    REQUIRE(m.next_display() != nullptr);
    CHECK(m.next_display()->pts90k == 3000);
}

TEST_CASE("movie ES model: reset forgets the stream", "[movie]") {
    const Fed f = gop_stream(4, 2);
    MpegEsModel m;
    m.feed(f.es);
    CHECK(m.ended());
    m.feed(f.es); // after the end: ignored
    CHECK(m.completed() == 4);
    CHECK(m.bytes_ignored() >= f.es.size());
    m.reset();
    CHECK_FALSE(m.ended());
    CHECK(m.completed() == 0);
    m.feed(f.es);
    CHECK(m.completed() == 4);
    CHECK(m.take_coded()->index == 0);
}

TEST_CASE("movie ES model: streams the HLE cannot follow are refused", "[movie]") {
    SECTION("a picture before any sequence header") {
        const Fed f = gop_stream(2, 0);
        const std::vector<std::size_t> starts = mpeg2_picture_offsets(f.es);
        MpegEsModel m;
        m.feed(std::span<const std::uint8_t>(f.es).subspan(starts[0]));
        CHECK(m.error().find("before the first sequence header") != std::string::npos);
    }
    SECTION("field pictures") {
        Fed f = gop_stream(2, 0);
        // picture_structure is the low two bits of the third byte after the picture coding extension's code.
        const std::vector<std::size_t> starts = mpeg2_picture_offsets(f.es);
        std::size_t ext = starts[0];
        while (!(f.es[ext] == 0 && f.es[ext + 1] == 0 && f.es[ext + 2] == 1 && f.es[ext + 3] == 0xB5)) {
            ++ext;
        }
        f.es[ext + 6] = static_cast<std::uint8_t>((f.es[ext + 6] & ~3u) | 1u); // top field
        MpegEsModel m;
        m.feed(f.es);
        CHECK(m.error().find("picture_structure 1") != std::string::npos);
    }
    SECTION("4:2:2") {
        Fed f = gop_stream(2, 0);
        f.es[17] =
            static_cast<std::uint8_t>((f.es[17] & ~6u) | (2u << 1)); // the sequence extension's chroma_format
        MpegEsModel m;
        m.feed(f.es);
        CHECK(m.error().find("chroma_format 2") != std::string::npos);
    }
    SECTION("a stream with no picture boundary") {
        MpegEsModel m;
        const Fed f = gop_stream(1, 0, false);
        m.feed(f.es);
        const std::vector<std::uint8_t> junk(MpegEsModel::max_picture_bytes, 0xAB);
        m.feed(junk);
        CHECK(m.error().find("no picture boundary") != std::string::npos);
        const std::uint64_t before = m.bytes_ignored();
        m.feed(f.es);
        CHECK(m.bytes_ignored() == before + f.es.size());
    }
}

TEST_CASE("movie ES model: helpers read one coded picture", "[movie]") {
    const Fed f = gop_stream(3, 1); // I0 P2 B1
    MpegEsModel m;
    m.feed(f.es);
    const auto first = m.take_coded();
    REQUIRE(first);
    const auto seq = mpeg_sequence_header(first->bytes);
    REQUIRE(seq);
    CHECK(seq->width == 48);
    CHECK(seq->height == 32);
    CHECK(seq->mpeg2);
    CHECK_FALSE(mpeg_sequence_header(m.take_coded()->bytes)); // only the first picture carries the headers
    CHECK(mpeg_picture_type(m.take_coded()->bytes) == MpegPictureType::b);
    const std::uint8_t none[] = {0, 0, 1, 0xB3};
    CHECK_FALSE(mpeg_picture_type(none));
}
