// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// The start-code scan and the sequence header and extension fields are adapted from PS2Recomp
// (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors: ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp
// (findStartCode, findMpegSequenceHeader, updateMpegPictureTiming).
#include "runtime/movie/mpeg_es_model.h"

#include <utility>

namespace dq8::rt {

namespace {

constexpr std::uint8_t code_picture = 0x00;
constexpr std::uint8_t code_sequence_header = 0xB3;
constexpr std::uint8_t code_extension = 0xB5;
constexpr std::uint8_t code_sequence_end = 0xB7;
constexpr std::uint8_t code_gop = 0xB8;

constexpr std::uint8_t extension_sequence = 0x1;
constexpr std::uint8_t extension_picture_coding = 0x8;
constexpr std::uint32_t structure_frame = 3;
constexpr std::uint32_t chroma_420 = 1;

// Bytes from the start code's first byte that must be present before the code is acted on.
std::size_t bytes_needed(std::uint8_t code) noexcept {
    switch (code) {
    case code_picture:
        return 6; // temporal_reference and picture_coding_type
    case code_sequence_header:
        return 8; // size, aspect ratio and frame rate
    case code_extension:
        return 10; // the longest field used: the sequence extension's low_delay
    default:
        return 4;
    }
}

bool start_code_at(std::span<const std::uint8_t> b, std::size_t i) noexcept {
    return b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1;
}

std::optional<MpegPictureType> picture_type_at(std::span<const std::uint8_t> b, std::size_t at) noexcept {
    switch ((b[at + 5] >> 3) & 7u) {
    case 1:
        return MpegPictureType::i;
    case 2:
        return MpegPictureType::p;
    case 3:
        return MpegPictureType::b;
    default:
        return std::nullopt; // 0 is forbidden, 4 is an MPEG-1 D picture
    }
}

void read_sequence_header(std::span<const std::uint8_t> b, std::size_t at, MpegSequenceInfo& s) noexcept {
    s.known = true;
    s.width = (std::uint32_t{b[at + 4]} << 4) | (b[at + 5] >> 4);
    s.height = ((std::uint32_t{b[at + 5]} & 0x0Fu) << 8) | b[at + 6];
    s.frame_rate_code = b[at + 7] & 0x0Fu;
    s.mpeg2 = false;
    s.low_delay = false;
}

// The sequence extension: chroma_format, the size extensions and low_delay. Returns chroma_format.
std::uint32_t read_sequence_extension(std::span<const std::uint8_t> b, std::size_t at,
                                      MpegSequenceInfo& s) noexcept {
    const std::uint32_t chroma = (b[at + 5] >> 1) & 3u;
    const std::uint32_t h_ext = ((b[at + 5] & 1u) << 1) | (b[at + 6] >> 7);
    const std::uint32_t v_ext = (b[at + 6] >> 5) & 3u;
    s.width = (s.width & 0x0FFFu) | (h_ext << 12);
    s.height = (s.height & 0x0FFFu) | (v_ext << 12);
    s.low_delay = (b[at + 9] & 0x80u) != 0;
    s.mpeg2 = true;
    return chroma;
}

} // namespace

void MpegEsModel::reset() {
    *this = MpegEsModel{};
}

void MpegEsModel::fail(std::string message) {
    if (error_.empty()) {
        error_ = std::move(message);
    }
}

void MpegEsModel::feed(std::span<const std::uint8_t> bytes, std::int64_t pts90k, std::int64_t dts90k) {
    if (bytes.empty()) {
        return;
    }
    if (ended_ || !error_.empty()) {
        ignored_ += bytes.size();
        stream_offset_ += bytes.size();
        return;
    }
    if (pts90k >= 0 || dts90k >= 0) {
        stamps_.push_back({stream_offset_, pts90k, dts90k});
    }
    unit_.insert(unit_.end(), bytes.begin(), bytes.end());
    stream_offset_ += bytes.size();
    scan();
    if (error_.empty() && unit_.size() > max_picture_bytes) {
        fail("a coded picture longer than " + std::to_string(max_picture_bytes) +
             " bytes: the video stream has no picture boundary there");
    }
}

void MpegEsModel::end_of_stream() {
    if (ended_ || !error_.empty()) {
        return;
    }
    if (picture_at_) {
        complete_unit(unit_.size());
    }
    ignored_ += unit_.size();
    unit_.clear();
    ended_ = true;
    if (held_) {
        display_.push_back(*held_);
        held_.reset();
    }
}

void MpegEsModel::scan() {
    std::size_t i = scanned_;
    while (error_.empty() && !ended_) {
        const std::span<const std::uint8_t> b(unit_);
        std::size_t found = b.size();
        for (; i + 3 < b.size(); ++i) {
            if (start_code_at(b, i)) {
                found = i;
                break;
            }
        }
        if (found == b.size()) {
            scanned_ = i;
            return;
        }
        const std::uint8_t code = b[found + 3];
        if (found + bytes_needed(code) > b.size()) {
            scanned_ = found; // wait for the rest of the header
            return;
        }
        std::size_t at = found;
        const bool boundary = code == code_picture || code == code_sequence_header || code == code_gop ||
                              code == code_sequence_end;
        if (boundary && picture_at_) {
            complete_unit(found);
            at = 0;
            if (!error_.empty()) {
                return;
            }
        }
        const std::span<const std::uint8_t> u(unit_);
        switch (code) {
        case code_picture: {
            if (!sequence_.known) {
                fail("a picture before the first sequence header");
                return;
            }
            if (!picture_type_at(u, at)) {
                fail("picture_coding_type " + std::to_string((u[at + 5] >> 3) & 7u) +
                     " (only I, P and B pictures)");
                return;
            }
            picture_at_ = at;
            unit_pts_ = -1;
            unit_dts_ = -1;
            // The timestamps of the PES packet in which this start code begins (ISO/IEC 13818-1).
            const std::uint64_t offset = unit_offset_ + at;
            while (stamps_.size() >= 2 && stamps_[1].offset <= offset) {
                stamps_.pop_front();
            }
            if (!stamps_.empty() && stamps_.front().offset <= offset) {
                unit_pts_ = stamps_.front().pts90k;
                unit_dts_ = stamps_.front().dts90k;
                stamps_.pop_front();
            }
            break;
        }
        case code_sequence_header:
            parse_sequence_header(at);
            break;
        case code_extension:
            parse_extension(at);
            break;
        case code_sequence_end:
            ended_ = true;
            ignored_ += unit_.size(); // the end code and anything left before it that is not a picture
            unit_offset_ += unit_.size();
            unit_.clear();
            scanned_ = 0;
            if (held_) {
                display_.push_back(*held_);
                held_.reset();
            }
            return;
        default:
            break; // slices, user data, GOP headers: part of the unit
        }
        i = at + 4;
    }
}

void MpegEsModel::parse_sequence_header(std::size_t at) {
    read_sequence_header(unit_, at, sequence_);
    if (sequence_.width == 0 || sequence_.height == 0) {
        fail("a sequence header with a zero picture size");
    }
}

void MpegEsModel::parse_extension(std::size_t at) {
    const std::span<const std::uint8_t> b(unit_);
    const std::uint8_t id = b[at + 4] >> 4;
    if (id == extension_sequence) {
        if (!sequence_.known) {
            fail("a sequence extension before the first sequence header");
            return;
        }
        const std::uint32_t chroma = read_sequence_extension(b, at, sequence_);
        if (chroma != chroma_420) {
            fail("chroma_format " + std::to_string(chroma) + " (the movie HLE handles 4:2:0 only)");
        }
    } else if (id == extension_picture_coding && picture_at_) {
        const std::uint32_t structure = b[at + 6] & 3u;
        if (structure != structure_frame) {
            fail("picture_structure " + std::to_string(structure) +
                 " (field pictures; the movie HLE handles frame pictures only)");
        }
    }
}

void MpegEsModel::complete_unit(std::size_t end) {
    const std::span<const std::uint8_t> b(unit_);
    const auto type = picture_type_at(b, *picture_at_);
    MpegCodedPicture pic;
    pic.index = next_index_++;
    pic.type = *type; // checked when the start code was seen
    pic.pts90k = unit_pts_;
    pic.dts90k = unit_dts_;
    pic.bytes.assign(unit_.begin(), unit_.begin() + static_cast<std::ptrdiff_t>(end));
    unit_.erase(unit_.begin(), unit_.begin() + static_cast<std::ptrdiff_t>(end));
    unit_offset_ += end;
    scanned_ = 0;
    picture_at_.reset();
    unit_pts_ = unit_dts_ = -1;

    const MpegDisplayPicture shown{pic.index, pic.type, pic.pts90k, pic.dts90k};
    coded_.push_back(std::move(pic));
    ++completed_;
    if (sequence_.low_delay || shown.type == MpegPictureType::b) {
        display_.push_back(shown);
    } else {
        if (held_) {
            display_.push_back(*held_);
        }
        held_ = shown;
    }
}

std::optional<MpegCodedPicture> MpegEsModel::take_coded() {
    if (coded_.empty()) {
        return std::nullopt;
    }
    MpegCodedPicture pic = std::move(coded_.front());
    coded_.pop_front();
    return pic;
}

void MpegEsModel::pop_display() {
    if (!display_.empty()) {
        display_.pop_front();
        ++delivered_;
    }
}

std::optional<MpegPictureType> mpeg_picture_type(std::span<const std::uint8_t> coded) {
    for (std::size_t i = 0; i + 6 <= coded.size(); ++i) {
        if (start_code_at(coded, i) && coded[i + 3] == code_picture) {
            return picture_type_at(coded, i);
        }
    }
    return std::nullopt;
}

std::optional<MpegSequenceInfo> mpeg_sequence_header(std::span<const std::uint8_t> coded) {
    std::optional<MpegSequenceInfo> out;
    for (std::size_t i = 0; i + 4 <= coded.size(); ++i) {
        if (!start_code_at(coded, i)) {
            continue;
        }
        const std::uint8_t code = coded[i + 3];
        if (code == code_picture) {
            break;
        }
        if (code == code_sequence_header && i + bytes_needed(code) <= coded.size()) {
            out.emplace();
            read_sequence_header(coded, i, *out);
        } else if (code == code_extension && out && i + bytes_needed(code) <= coded.size() &&
                   (coded[i + 4] >> 4) == extension_sequence) {
            read_sequence_extension(coded, i, *out);
        }
    }
    return out;
}

} // namespace dq8::rt
