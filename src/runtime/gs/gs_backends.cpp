// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/gs/gs_backends.h"

#include "runtime/gs/gs_trace_format.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace dq8::gs {

namespace {

class NullBackend final : public IGsBackend {
  public:
    GsCaps caps() const override { return {"null", false, false, 1}; }
    void reset() override {}
    void gif_transfer(GifPath /*path*/, std::span<const std::uint8_t> /*qwords*/) override {}
    void vsync(const VsyncInfo& /*info*/) override {}
    bool read_frame(FrameImage& /*out*/) override { return false; }
    std::size_t read_transfer(std::span<std::uint8_t> out) override {
        std::fill(out.begin(), out.end(), std::uint8_t{0});
        return out.size();
    }
    void flush() override {}
};

class TraceBackend final : public IGsBackend {
  public:
    explicit TraceBackend(std::function<void(std::span<const std::uint8_t>)> sink) : sink_(std::move(sink)) {}

    GsCaps caps() const override { return {"trace", false, false, 1}; }
    void reset() override {}

    void gif_transfer(GifPath path, std::span<const std::uint8_t> qwords) override {
        buffer_.clear();
        trace::transfer_header(buffer_, path, qwords.size());
        sink_(buffer_);
        sink_(qwords);
    }

    void vsync(const VsyncInfo& info) override {
        buffer_.clear();
        trace::vsync_records(buffer_, info);
        sink_(buffer_);
    }

    bool read_frame(FrameImage& /*out*/) override { return false; }

    std::size_t read_transfer(std::span<std::uint8_t> out) override {
        buffer_.clear();
        trace::read_fifo_record(buffer_, out.size());
        sink_(buffer_);
        std::fill(out.begin(), out.end(), std::uint8_t{0});
        return out.size();
    }

    void flush() override { sink_({}); }

  private:
    std::function<void(std::span<const std::uint8_t>)> sink_;
    std::vector<std::uint8_t> buffer_;
};

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};

} // namespace

std::unique_ptr<IGsBackend> make_null_backend() {
    return std::make_unique<NullBackend>();
}

std::unique_ptr<IGsBackend> make_trace_backend(std::function<void(std::span<const std::uint8_t>)> sink) {
    if (!sink) {
        throw std::invalid_argument("trace backend: no sink");
    }
    return std::make_unique<TraceBackend>(std::move(sink));
}

std::unique_ptr<IGsBackend> make_trace_backend(const std::string& path) {
    std::shared_ptr<std::FILE> file(std::fopen(path.c_str(), "wb"), FileCloser{});
    if (!file) {
        throw std::runtime_error("trace backend: cannot open " + path + " for writing");
    }
    return make_trace_backend([file](std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            std::fflush(file.get()); // flush() passes an empty chunk
            return;
        }
        if (std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
            throw std::runtime_error("trace backend: write failed");
        }
    });
}

} // namespace dq8::gs
