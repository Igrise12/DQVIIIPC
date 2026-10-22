// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The MPEG-2 video elementary-stream model of the sceMpeg HLE (architecture §7.2, ADR-0015,
// Docs/milestones/M4-playable.md, session C). It splits the stream the game's demux hands over into coded
// pictures and orders them for display; it alone decides how many pictures a movie has, which ones can be
// delivered now and with which timestamps. A decoder only paints the pixels of a picture this model has
// already counted, so runs with and without a real decoder deliver the same pictures at the same times.
//
// A coded picture is the picture header with its extensions and slices, together with the sequence and GOP
// headers in front of it (the split FFmpeg's own MPEG video parser makes). It is complete when the next
// picture, sequence header, GOP header or sequence end code begins. Display order follows the reordering of
// reference pictures in ISO/IEC 13818-2: a B picture is shown as soon as it is complete; an I or P picture is
// held until the next I or P picture is complete (or the stream ends), unless the sequence is low-delay.
// Field pictures, 4:2:2 and 4:4:4 are refused (error()): the movie HLE handles progressive 4:2:0 frames.
namespace dq8::rt {

enum class MpegPictureType : std::uint8_t { i = 1, p = 2, b = 3 };

struct MpegSequenceInfo {
    bool known = false; // a sequence header has been seen
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_rate_code = 0;
    bool mpeg2 = false; // a sequence extension followed the header
    bool low_delay = false;
};

struct MpegCodedPicture {
    std::uint32_t index = 0; // decode order, from 0 after a reset
    MpegPictureType type = MpegPictureType::i;
    std::int64_t pts90k = -1; // from the PES packet in which the picture starts; -1 when it carried none
    std::int64_t dts90k = -1;
    std::vector<std::uint8_t> bytes;
};

// A picture in display order: which coded picture it is and its timestamps.
struct MpegDisplayPicture {
    std::uint32_t index = 0; // the coded picture's decode-order index
    MpegPictureType type = MpegPictureType::i;
    std::int64_t pts90k = -1;
    std::int64_t dts90k = -1;
};

class MpegEsModel {
  public:
    // The longest coded picture accepted before the stream is declared broken.
    static constexpr std::size_t max_picture_bytes = 4u << 20;

    // Appends elementary-stream bytes (one PES payload). The timestamps, when not negative, belong to the
    // first picture whose start code begins in these bytes. Bytes after a sequence end code are ignored until
    // reset(). On a stream the model cannot follow, error() becomes non-empty and the bytes are dropped.
    void feed(std::span<const std::uint8_t> bytes, std::int64_t pts90k = -1, std::int64_t dts90k = -1);
    // The stream ended without a sequence end code: the picture in progress is complete.
    void end_of_stream();
    void reset();

    const MpegSequenceInfo& sequence() const noexcept { return sequence_; }
    const std::string& error() const noexcept { return error_; }
    // A sequence end code was seen (or end_of_stream()).
    bool ended() const noexcept { return ended_; }
    // Every picture of the stream has been delivered: ended() and nothing left to show.
    bool finished() const noexcept { return ended_ && display_.empty() && !held_; }

    // Complete coded pictures not yet taken for the decoder, oldest first (decode order).
    std::size_t coded_waiting() const noexcept { return coded_.size(); }
    std::optional<MpegCodedPicture> take_coded();

    // The next picture to deliver in display order, if one is ready.
    const MpegDisplayPicture* next_display() const noexcept {
        return display_.empty() ? nullptr : &display_.front();
    }
    // Marks next_display() delivered.
    void pop_display();

    // Complete pictures not yet delivered: the backpressure count.
    std::uint64_t pending() const noexcept { return completed_ - delivered_; }
    std::uint64_t completed() const noexcept { return completed_; }
    std::uint64_t delivered() const noexcept { return delivered_; }
    std::uint64_t bytes_fed() const noexcept { return stream_offset_; }
    std::uint64_t bytes_ignored() const noexcept { return ignored_; }

  private:
    void scan();
    void complete_unit(std::size_t end);
    void fail(std::string message);
    void parse_sequence_header(std::size_t at);
    void parse_extension(std::size_t at);

    MpegSequenceInfo sequence_;
    std::string error_;
    bool ended_ = false;

    std::vector<std::uint8_t> unit_;        // the picture in progress (and the headers before it)
    std::uint64_t unit_offset_ = 0;         // stream offset of unit_[0]
    std::size_t scanned_ = 0;               // unit_ bytes searched for start codes so far
    std::optional<std::size_t> picture_at_; // the picture start code in unit_
    std::int64_t unit_pts_ = -1;
    std::int64_t unit_dts_ = -1;

    // Timestamps of the latest PES payload, waiting for a picture to start inside it.
    struct PesStamp {
        std::uint64_t offset = 0;
        std::int64_t pts90k = -1;
        std::int64_t dts90k = -1;
    };
    std::deque<PesStamp> stamps_;

    std::uint64_t stream_offset_ = 0;
    std::uint64_t ignored_ = 0;
    std::uint32_t next_index_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t delivered_ = 0;
    std::deque<MpegCodedPicture> coded_;
    std::deque<MpegDisplayPicture> display_;
    std::optional<MpegDisplayPicture> held_;
};

// The picture coding type of one coded picture as the model splits them, or nullopt if it holds no complete
// picture header. Decoders that do not decode (--movies nodecode) use it to order their output.
std::optional<MpegPictureType> mpeg_picture_type(std::span<const std::uint8_t> coded);
// The sequence header in one coded picture, if it carries one (width, height, frame rate; low_delay and
// mpeg2 from the sequence extension after it).
std::optional<MpegSequenceInfo> mpeg_sequence_header(std::span<const std::uint8_t> coded);

} // namespace dq8::rt
