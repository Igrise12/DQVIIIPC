// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The decoder behind --movies nodecode: no pixels, the same pictures in the same order as a real decoder.
#include "runtime/movie/movie_decoder.h"

#include "runtime/movie/macroblock_writer.h"
#include "runtime/movie/mpeg_es_model.h"

#include <deque>
#include <optional>

namespace dq8::rt {

namespace {

// Orders the tags like an MPEG-2 decoder orders its output (the reference rule of mpeg_es_model.h) and hands
// out black pictures of the size the last sequence header gave.
class BlankMovieDecoder final : public IMovieDecoder {
  public:
    std::string name() const override { return "blank"; }

    void reset() override {
        ready_.clear();
        held_.reset();
    }

    void decode_picture(std::span<const std::uint8_t> coded, std::int64_t pts90k) override {
        if (const auto seq = mpeg_sequence_header(coded)) {
            width_ = seq->width;
            height_ = seq->height;
            low_delay_ = seq->low_delay;
        }
        const auto type = mpeg_picture_type(coded);
        if (!type) {
            throw MovieDecoderError("blank decoder: no picture header in a coded picture");
        }
        if (low_delay_ || *type == MpegPictureType::b) {
            ready_.push_back(pts90k);
            return;
        }
        if (held_) {
            ready_.push_back(*held_);
        }
        held_ = pts90k;
    }

    void flush() override {
        if (held_) {
            ready_.push_back(*held_);
            held_.reset();
        }
    }

    bool next_picture(PictureYuv& out) override {
        if (ready_.empty() || width_ == 0 || height_ == 0) {
            return false;
        }
        out = black_.get(width_, height_);
        out.pts90k = ready_.front();
        ready_.pop_front();
        return true;
    }

  private:
    std::deque<std::int64_t> ready_;
    std::optional<std::int64_t> held_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool low_delay_ = false;
    BlackPicture black_;
};

} // namespace

std::unique_ptr<IMovieDecoder> make_blank_movie_decoder() {
    return std::make_unique<BlankMovieDecoder>();
}

} // namespace dq8::rt
