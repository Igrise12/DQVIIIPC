// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// GsPipeline: the producer-side hash chain, the render thread against inline mode, read_transfer, the frame
// mailbox and PNG dumps.
#include "dq8/core/hash.h"
#include "runtime/gs/gs_backends.h"
#include "runtime/gs/gs_pipeline.h"
#include "runtime/gs/gs_trace_format.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dq8;

namespace {

// Records what reaches it (on the render thread) and draws a tiny frame whose pixels name the field.
class RecordingBackend final : public gs::IGsBackend {
  public:
    struct Log {
        std::vector<std::string> calls;
        std::thread::id thread;
    };
    explicit RecordingBackend(std::shared_ptr<Log> log, bool pixels = false)
        : log_(std::move(log)), pixels_(pixels) {}

    gs::GsCaps caps() const override { return {"recording", pixels_, true, 1}; }
    void reset() override {}
    void gif_transfer(gs::GifPath path, std::span<const std::uint8_t> qwords) override {
        note("transfer " + std::to_string(static_cast<int>(path)) + " " + std::to_string(qwords.size()));
        if (fail_on_transfer) {
            throw std::runtime_error("backend failure");
        }
    }
    void vsync(const gs::VsyncInfo& info) override {
        note("vsync " + std::to_string(info.field));
        field_ = info.field;
    }
    bool read_frame(gs::FrameImage& out) override {
        if (!pixels_) {
            return false;
        }
        out.width = 4;
        out.height = 2;
        out.rgba.assign(4 * 2 * 4, static_cast<std::uint8_t>(field_));
        return true;
    }
    std::size_t read_transfer(std::span<std::uint8_t> out) override {
        note("read " + std::to_string(out.size()));
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::uint8_t>(i + 1);
        }
        return out.size();
    }
    void flush() override {}

    bool fail_on_transfer = false;

  private:
    void note(std::string s) {
        log_->calls.push_back(std::move(s));
        log_->thread = std::this_thread::get_id();
    }
    std::shared_ptr<Log> log_;
    bool pixels_;
    std::uint64_t field_ = 0;
};

// Feeds the same synthetic stream to a pipeline: three fields of transfers on every path.
void feed(gs::GsPipeline& pipe, std::uint64_t fields) {
    for (std::uint64_t f = 0; f < fields; ++f) {
        for (std::uint8_t p = 1; p <= 3; ++p) {
            std::vector<std::uint8_t> packet(16u * (f + p), static_cast<std::uint8_t>(f * 3 + p));
            pipe.gif_transfer(static_cast<gs::GifPath>(p), packet);
        }
        gs::VsyncInfo info;
        info.field = f;
        info.odd_field = (f & 1) != 0;
        info.priv.write64(gs::PrivReg::csr, info.odd_field ? gs::csr_field : 0);
        pipe.vsync(info);
    }
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

} // namespace

TEST_CASE("gs pipeline: the stream hash chains SHA-1 over the trace bytes of each field", "[gs][pipeline]") {
    gs::GsPipelineConfig cfg;
    cfg.backend = gs::make_null_gs_backend();
    cfg.inline_mode = true;
    gs::GsPipeline pipe(std::move(cfg));
    CHECK(pipe.stream_hash() == std::string(40, '0'));
    CHECK(pipe.caps().name == "null");

    // The same records, hashed by hand: h_n = SHA1(h_{n-1} || records of field n).
    core::Sha1::Digest h{};
    for (std::uint64_t f = 0; f < 3; ++f) {
        std::vector<std::uint8_t> records;
        for (std::uint8_t p = 1; p <= 3; ++p) {
            std::vector<std::uint8_t> packet(16u * (f + p), static_cast<std::uint8_t>(f * 3 + p));
            gs::trace::transfer_header(records, static_cast<gs::GifPath>(p), packet.size());
            records.insert(records.end(), packet.begin(), packet.end());
        }
        gs::VsyncInfo info;
        info.field = f;
        info.odd_field = (f & 1) != 0;
        info.priv.write64(gs::PrivReg::csr, info.odd_field ? gs::csr_field : 0);
        gs::trace::vsync_records(records, info);
        core::Sha1 sha;
        sha.update(h);
        sha.update(records);
        h = sha.finish();
    }
    feed(pipe, 3);
    CHECK(pipe.stream_hash() == hex(h));
    CHECK(pipe.vsyncs() == 3u);
}

TEST_CASE("gs pipeline: the render thread sees what inline mode sees, in order, off the machine thread",
          "[gs][pipeline]") {
    auto inline_log = std::make_shared<RecordingBackend::Log>();
    auto thread_log = std::make_shared<RecordingBackend::Log>();
    std::string inline_hash;
    std::string thread_hash;
    {
        gs::GsPipelineConfig cfg;
        cfg.backend = std::make_unique<RecordingBackend>(inline_log);
        cfg.inline_mode = true;
        gs::GsPipeline pipe(std::move(cfg));
        feed(pipe, 5);
        inline_hash = pipe.stream_hash();
    }
    {
        gs::GsPipelineConfig cfg;
        cfg.backend = std::make_unique<RecordingBackend>(thread_log);
        gs::GsPipeline pipe(std::move(cfg));
        feed(pipe, 5);
        thread_hash = pipe.stream_hash();
    } // drains and joins
    CHECK(thread_hash == inline_hash);
    CHECK(thread_log->calls == inline_log->calls);
    CHECK(thread_log->calls.size() == 5u * 4u);
    CHECK(inline_log->thread == std::this_thread::get_id());
    CHECK(thread_log->thread != std::this_thread::get_id());
}

TEST_CASE("gs pipeline: read_transfer waits for the backend's data, after everything queued before it",
          "[gs][pipeline]") {
    auto log = std::make_shared<RecordingBackend::Log>();
    gs::GsPipelineConfig cfg;
    cfg.backend = std::make_unique<RecordingBackend>(log);
    gs::GsPipeline pipe(std::move(cfg));
    feed(pipe, 2);
    std::vector<std::uint8_t> out(32, 0);
    CHECK(pipe.read_transfer(out) == 32u);
    CHECK(out[0] == 1u);
    CHECK(out[31] == 32u);
    // The read is a barrier: the backend has seen the whole stream before it.
    REQUIRE(log->calls.size() == 2u * 4u + 1u);
    CHECK(log->calls.back() == "read 32");
}

TEST_CASE("gs pipeline: frames reach the mailbox and requested fields are dumped as PNG", "[gs][pipeline]") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path png = dir / "dq8_gs_pipeline_test_field2.png";
    const std::filesystem::path never = dir / "dq8_gs_pipeline_test_field9.png";
    std::filesystem::remove(png);
    auto log = std::make_shared<RecordingBackend::Log>();
    gs::GsPipelineConfig cfg;
    cfg.backend = std::make_unique<RecordingBackend>(log, true);
    gs::GsPipeline pipe(std::move(cfg));
    gs::FrameImage frame;
    CHECK_FALSE(pipe.latest_frame(frame));
    pipe.request_frame_dump(2, png.string());
    pipe.request_frame_dump(9, never.string());
    feed(pipe, 4);
    std::vector<std::uint8_t> barrier(16);
    pipe.read_transfer(barrier);
    REQUIRE(pipe.latest_frame(frame));
    CHECK(frame.field == 3u);
    CHECK(frame.width == 4u);
    CHECK(frame.rgba.front() == 3u);
    REQUIRE(std::filesystem::exists(png));
    std::ifstream in(png, std::ios::binary);
    char sig[8] = {};
    in.read(sig, 8);
    in.close();
    CHECK(std::string(sig + 1, 3) == "PNG");
    CHECK_FALSE(std::filesystem::exists(never));
    std::filesystem::remove(png);

    gs::FrameImage empty;
    CHECK_FALSE(gs::write_png((dir / "dq8_empty.png").string(), empty));
}

TEST_CASE("gs pipeline: a small queue applies backpressure without losing or reordering data",
          "[gs][pipeline]") {
    auto log = std::make_shared<RecordingBackend::Log>();
    gs::GsPipelineConfig cfg;
    cfg.backend = std::make_unique<RecordingBackend>(log);
    cfg.queue_bytes = 1; // raised to the 1 MiB floor
    gs::GsPipeline pipe(std::move(cfg));
    const std::vector<std::uint8_t> big(256 * 1024, 0x5A);
    for (int i = 0; i < 64; ++i) {
        pipe.gif_transfer(gs::GifPath::path3, big);
    }
    std::vector<std::uint8_t> barrier(16);
    pipe.read_transfer(barrier);
    REQUIRE(log->calls.size() == 65u);
    CHECK(log->calls[63] == "transfer 3 262144");
}

TEST_CASE("gs pipeline: a backend failure on the render thread reaches the machine thread",
          "[gs][pipeline]") {
    auto log = std::make_shared<RecordingBackend::Log>();
    auto backend = std::make_unique<RecordingBackend>(log);
    backend->fail_on_transfer = true;
    gs::GsPipelineConfig cfg;
    cfg.backend = std::move(backend);
    gs::GsPipeline pipe(std::move(cfg));
    const std::vector<std::uint8_t> packet(16, 0);
    pipe.gif_transfer(gs::GifPath::path1, packet);
    std::vector<std::uint8_t> barrier(16);
    CHECK_THROWS_AS(pipe.read_transfer(barrier), std::runtime_error);
    CHECK_THROWS_AS(gs::GsPipeline(gs::GsPipelineConfig{}), std::invalid_argument);
}
