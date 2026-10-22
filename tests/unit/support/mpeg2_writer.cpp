// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Written from ISO/IEC 13818-2 (MPEG-2 video): the header syntax of §6.2 and the intra DC tables B.12 and
// B.13.
#include "support/mpeg2_writer.h"

#include <cstdlib>
#include <stdexcept>

namespace dq8::test {

namespace {

class BitWriter {
  public:
    void put(std::uint32_t value, unsigned bits) {
        for (unsigned i = bits; i-- > 0;) {
            acc_ = static_cast<std::uint8_t>((static_cast<std::uint32_t>(acc_) << 1) | ((value >> i) & 1u));
            if (++used_ == 8) {
                out_.push_back(acc_);
                acc_ = 0;
                used_ = 0;
            }
        }
    }
    // Zero bits up to the next byte boundary (the stuffing before a start code).
    void align() {
        while (used_ != 0) {
            put(0, 1);
        }
    }
    void start_code(std::uint8_t code) {
        align();
        out_.push_back(0);
        out_.push_back(0);
        out_.push_back(1);
        out_.push_back(code);
    }
    std::vector<std::uint8_t>& bytes() {
        align();
        return out_;
    }

  private:
    std::vector<std::uint8_t> out_;
    std::uint8_t acc_ = 0;
    unsigned used_ = 0;
};

struct Vlc {
    std::uint32_t code;
    unsigned bits;
};

// dct_dc_size_luminance (Table B.12) and dct_dc_size_chrominance (Table B.13), sizes 0 to 11.
constexpr Vlc dc_size_luma[12] = {{0b100, 3},     {0b00, 2},       {0b01, 2},        {0b101, 3},
                                  {0b110, 3},     {0b1110, 4},     {0b11110, 5},     {0b111110, 6},
                                  {0b1111110, 7}, {0b11111110, 8}, {0b111111110, 9}, {0b111111111, 9}};
constexpr Vlc dc_size_chroma[12] = {
    {0b00, 2},     {0b01, 2},      {0b10, 2},       {0b110, 3},       {0b1110, 4},        {0b11110, 5},
    {0b111110, 6}, {0b1111110, 7}, {0b11111110, 8}, {0b111111110, 9}, {0b1111111110, 10}, {0b1111111111, 10}};

unsigned bit_length(std::uint32_t v) {
    unsigned n = 0;
    while (v != 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

// One intra block with only its DC coefficient (intra_dc_precision 0: the block's value is the DC level),
// then end of block ('10' in Table B.14).
void intra_dc_block(BitWriter& w, bool luma, int value, int& predictor) {
    const int diff = value - predictor;
    predictor = value;
    const unsigned size = bit_length(static_cast<std::uint32_t>(std::abs(diff)));
    const Vlc& v = luma ? dc_size_luma[size] : dc_size_chroma[size];
    w.put(v.code, v.bits);
    if (size != 0) {
        const int coded = diff < 0 ? diff + (1 << size) - 1 : diff;
        w.put(static_cast<std::uint32_t>(coded), size);
    }
    w.put(0b10, 2);
}

std::uint32_t coding_type(char type) {
    switch (type) {
    case 'I':
        return 1;
    case 'P':
        return 2;
    case 'B':
        return 3;
    default:
        throw std::invalid_argument("mpeg2 writer: picture type must be I, P or B");
    }
}

void sequence_headers(BitWriter& w, const Mpeg2TestStream& s) {
    w.start_code(0xB3);
    w.put(s.width & 0xFFFu, 12);
    w.put(s.height & 0xFFFu, 12);
    w.put(1, 4);      // aspect_ratio_information: square samples
    w.put(5, 4);      // frame_rate_code: 30 frames per second
    w.put(10000, 18); // bit_rate_value: 4 Mbit/s in 400 bit/s units
    w.put(1, 1);      // marker
    w.put(80, 10);    // vbv_buffer_size_value
    w.put(0, 1);      // constrained_parameters_flag
    w.put(0, 1);      // load_intra_quantiser_matrix
    w.put(0, 1);      // load_non_intra_quantiser_matrix

    w.start_code(0xB5);
    w.put(0x1, 4);  // sequence extension
    w.put(0x48, 8); // Main profile, Main level
    w.put(1, 1);    // progressive_sequence
    w.put(1, 2);    // chroma_format 4:2:0
    w.put(0, 2);    // horizontal_size_extension
    w.put(0, 2);    // vertical_size_extension
    w.put(0, 12);   // bit_rate_extension
    w.put(1, 1);    // marker
    w.put(0, 8);    // vbv_buffer_size_extension
    w.put(s.low_delay ? 1u : 0u, 1);
    w.put(0, 2); // frame_rate_extension_n
    w.put(0, 5); // frame_rate_extension_d
}

void gop_header(BitWriter& w) {
    w.start_code(0xB8);
    w.put(0, 1); // drop_frame_flag
    w.put(0, 5); // hours
    w.put(0, 6); // minutes
    w.put(1, 1); // marker
    w.put(0, 6); // seconds
    w.put(0, 6); // pictures
    w.put(1, 1); // closed_gop
    w.put(0, 1); // broken_link
}

void picture(BitWriter& w, const Mpeg2TestStream& s, const Mpeg2TestPicture& p) {
    const std::uint32_t type = coding_type(p.type);
    w.start_code(0x00);
    w.put(p.temporal_reference & 0x3FFu, 10);
    w.put(type, 3);
    w.put(0xFFFF, 16); // vbv_delay: variable bit rate
    if (type == 2 || type == 3) {
        w.put(0, 1); // full_pel_forward_vector
        w.put(7, 3); // forward_f_code ('111' in MPEG-2)
    }
    if (type == 3) {
        w.put(0, 1); // full_pel_backward_vector
        w.put(7, 3); // backward_f_code
    }
    w.put(0, 1); // extra_bit_picture

    w.start_code(0xB5);
    w.put(0x8, 4); // picture coding extension
    const std::uint32_t forward = type == 1 ? 15u : 1u;
    const std::uint32_t backward = type == 3 ? 1u : 15u;
    w.put(forward, 4);
    w.put(forward, 4);
    w.put(backward, 4);
    w.put(backward, 4);
    w.put(0, 2); // intra_dc_precision: 8 bits
    w.put(3, 2); // picture_structure: frame
    w.put(0, 1); // top_field_first
    w.put(1, 1); // frame_pred_frame_dct
    w.put(0, 1); // concealment_motion_vectors
    w.put(0, 1); // q_scale_type
    w.put(0, 1); // intra_vlc_format
    w.put(0, 1); // alternate_scan
    w.put(0, 1); // repeat_first_field
    w.put(1, 1); // chroma_420_type
    w.put(1, 1); // progressive_frame
    w.put(0, 1); // composite_display_flag

    const std::uint32_t columns = (s.width + 15) / 16;
    const std::uint32_t rows = (s.height + 15) / 16;
    for (std::uint32_t mby = 0; mby < rows; ++mby) {
        w.start_code(static_cast<std::uint8_t>(mby + 1)); // one slice per macroblock row
        w.put(8, 5);                                      // quantiser_scale_code
        w.put(0, 1);                                      // extra_bit_slice
        int pred_y = 128;
        int pred_cb = 128;
        int pred_cr = 128;
        for (std::uint32_t mbx = 0; mbx < columns; ++mbx) {
            w.put(1, 1); // macroblock_address_increment 1
            if (type == 1) {
                w.put(0b1, 1); // macroblock_type: intra (Table B.2)
            } else {
                w.put(0b00011, 5); // macroblock_type: intra (Tables B.3 and B.4)
            }
            const int y = mpeg2_test_luma(p.seed, mbx, mby);
            for (int b = 0; b < 4; ++b) {
                intra_dc_block(w, true, y, pred_y);
            }
            intra_dc_block(w, false, p.cb, pred_cb);
            intra_dc_block(w, false, p.cr, pred_cr);
        }
    }
}

} // namespace

std::uint8_t mpeg2_test_luma(std::uint32_t seed, std::uint32_t mbx, std::uint32_t mby) {
    return static_cast<std::uint8_t>(16u + (seed * 37u + mbx * 11u + mby * 23u) % 220u);
}

std::vector<std::uint8_t> write_mpeg2_stream(const Mpeg2TestStream& stream) {
    if (stream.width == 0 || stream.height == 0 || stream.width > 4095 || stream.height > 2800) {
        throw std::invalid_argument("mpeg2 writer: unsupported picture size");
    }
    BitWriter w;
    sequence_headers(w, stream);
    for (std::size_t i = 0; i < stream.pictures.size(); ++i) {
        const Mpeg2TestPicture& p = stream.pictures[i];
        if (i != 0 && p.gop_header) {
            sequence_headers(w, stream);
        }
        if (p.gop_header) {
            gop_header(w);
        }
        picture(w, stream, p);
    }
    if (stream.sequence_end) {
        w.start_code(0xB7);
    }
    return w.bytes();
}

std::vector<Mpeg2TestPicture> mpeg2_test_gop(std::uint32_t count, std::uint32_t b_frames,
                                             std::vector<std::uint32_t>& display_seeds) {
    display_seeds.clear();
    std::vector<char> types(count, 'B');
    for (std::uint32_t i = 0; i < count; ++i) {
        display_seeds.push_back(i + 1);
        if (i == 0) {
            types[i] = 'I';
        } else if (i % (b_frames + 1) == 0 || i + 1 == count) {
            types[i] = 'P';
        }
    }
    const auto make = [&](std::uint32_t i) {
        Mpeg2TestPicture p;
        p.type = types[i];
        p.seed = i + 1;
        p.cb = static_cast<std::uint8_t>(96 + (i * 9) % 64);
        p.cr = static_cast<std::uint8_t>(160 - (i * 5) % 64);
        p.temporal_reference = i;
        p.gop_header = i == 0;
        return p;
    };
    std::vector<Mpeg2TestPicture> out;
    std::uint32_t previous_reference = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (types[i] == 'B') {
            continue;
        }
        out.push_back(make(i));
        for (std::uint32_t b = previous_reference + 1; i != 0 && b < i; ++b) {
            out.push_back(make(b));
        }
        previous_reference = i;
    }
    return out;
}

std::vector<std::size_t> mpeg2_picture_offsets(std::span<const std::uint8_t> es) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i + 3 < es.size(); ++i) {
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && es[i + 3] == 0) {
            out.push_back(i);
        }
    }
    return out;
}

} // namespace dq8::test
