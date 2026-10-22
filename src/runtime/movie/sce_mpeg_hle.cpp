// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// The sceMpeg structure fields GetPicture fills (+0 width, +4 height, +8 the index of the picture delivered)
// and the RGB32 delivery are adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp (sceMpegGetPicture, sceMpegCreate).
#include "runtime/movie/sce_mpeg_hle.h"

#include "dq8/abi/ps2recomp/ps2_address.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/machine/clock.h"
#include "runtime/machine/cost_model.h"
#include "runtime/movie/ipu_regs.h"

#include <cstdlib>
#include <span>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

constexpr unsigned reg_v0 = 2;
constexpr unsigned reg_a0 = 4;
constexpr unsigned reg_t0 = 8;

// The sceMpeg structure (the SDK's public fields).
constexpr std::uint32_t mp_width = 0x00;
constexpr std::uint32_t mp_height = 0x04;
constexpr std::uint32_t mp_frame = 0x08;
constexpr std::uint32_t mp_pts = 0x10;
constexpr std::uint32_t mp_dts = 0x18;

// A callback's data block: +0 type; for a stream callback (DemuxPssRing) +8 the payload, +0xC its length,
// +0x10 PTS and +0x18 DTS (64-bit, negative when absent).
constexpr std::uint32_t cb_type = 0x00;
constexpr std::uint32_t cb_data = 0x08;
constexpr std::uint32_t cb_length = 0x0C;
constexpr std::uint32_t cb_pts = 0x10;
constexpr std::uint32_t cb_dts = 0x18;
constexpr std::uint32_t cb_block_bytes = 0x20;

constexpr std::uint32_t max_name_bytes = 255;

// Argument n of the call ($a0..$a3, then $t0..$t3).
std::uint32_t arg(const OverrideCall& call, unsigned n) {
    return gpr_u32(*call.ctx, n < 4 ? reg_a0 + n : reg_t0 + (n - 4));
}

void set_result(OverrideCall& call, std::int64_t value) {
    set_gpr_lo(*call.ctx, reg_v0, static_cast<std::uint64_t>(value));
    call.return_to_caller();
}

std::int64_t load_s64(GuestMemory& m, std::uint32_t at) {
    const std::uint64_t lo = m.load32(at);
    const std::uint64_t hi = m.load32(at + 4);
    return static_cast<std::int64_t>((hi << 32) | lo);
}

void store_s64(GuestMemory& m, std::uint32_t at, std::int64_t value) {
    const auto v = static_cast<std::uint64_t>(value);
    m.store32(at, static_cast<std::uint32_t>(v));
    m.store32(at + 4, static_cast<std::uint32_t>(v >> 32));
}

} // namespace

SceMpegHle::SceMpegHle(SceMpegHleConfig config) : config_(std::move(config)) {
    if (!config_.make_decoder) {
        throw std::invalid_argument("SceMpegHle: SceMpegHleConfig::make_decoder is required");
    }
    if (config_.max_pending_pictures < 2) {
        // One complete reference picture can be waiting for the next: fewer than two would never deliver it.
        throw std::invalid_argument("SceMpegHle: max_pending_pictures must be at least 2");
    }
}

SceMpegHle::~SceMpegHle() = default;

void SceMpegHle::attach(MachineServices& machine) {
    machine_ = &machine;
    ipu_ = IpuRegisters::find(machine);
}

void SceMpegHle::reset() {
    sessions_.clear();
    next_name_.clear();
    stats_ = Stats{};
}

void SceMpegHle::fail(const OverrideCall& call, std::string message) {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.pc = call.ctx != nullptr ? call.ctx->pc : 0;
    info.address = call.address;
    info.message = "sceMpeg HLE at " + hex32(call.address) + ": " + std::move(message);
    if (machine_ == nullptr) {
        throw GuestStop(std::move(info));
    }
    machine_->stop(std::move(info));
    std::abort(); // stop() never returns; GCC cannot see that through the virtual call
}

SceMpegHle::Session& SceMpegHle::session(const OverrideCall& call, std::uint32_t mp, std::string_view what) {
    const auto it = sessions_.find(mp);
    if (it == sessions_.end()) {
        fail(call, std::string(what) + " on mp " + hex32(mp) + ", which no sceMpegCreate set up");
    }
    return it->second;
}

void SceMpegHle::emit_movie(std::string_view phase, const Session& s) {
    machine_->emit(MachineEvent{"movie", {}}
                       .add("phase", std::string(phase))
                       .add("name", s.name)
                       .add("pictures", s.pictures)
                       .add("slot_a", hex32(s.slot_a))
                       .add("slot_b", hex32(s.slot_b)));
}

void SceMpegHle::hook_player_open(OverrideCall& call, unsigned name_argument) {
    const std::uint32_t at = arg(call, name_argument);
    GuestMemory& m = machine_->memory();
    std::string name;
    for (std::uint32_t i = 0; i < max_name_bytes; ++i) {
        const auto c = static_cast<char>(m.load8(at + i));
        if (c == '\0') {
            break;
        }
        name.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    next_name_ = name;
    Session announced;
    announced.name = std::move(name);
    emit_movie("open", announced);
}

void SceMpegHle::hook_create(OverrideCall& call) {
    const std::uint32_t mp = arg(call, 0);
    Session s;
    s.name = std::move(next_name_);
    next_name_.clear();
    try {
        s.decoder = config_.make_decoder();
    } catch (const MovieDecoderError& e) {
        fail(call, std::string("sceMpegCreate: no movie decoder: ") + e.what());
    }
    sessions_.insert_or_assign(mp, std::move(s));
    ++stats_.sessions;
}

void SceMpegHle::hook_delete(OverrideCall& call) {
    const auto it = sessions_.find(arg(call, 0));
    if (it == sessions_.end()) {
        return; // the SDK's Delete has nothing to undo either
    }
    emit_movie("delete", it->second);
    sessions_.erase(it);
}

void SceMpegHle::hook_add_callback(OverrideCall& call) {
    Session& s = session(call, arg(call, 0), "sceMpegAddCallback");
    if (arg(call, 1) == callback_nodata) {
        s.nodata = arg(call, 2);
        s.nodata_data = arg(call, 3);
    }
}

void SceMpegHle::hook_add_str_callback(OverrideCall& call) {
    session(call, arg(call, 0), "sceMpegAddStrCallback");
    const std::uint32_t function = arg(call, 3);
    if (arg(call, 1) == stream_video && function != config_.video_callback) {
        fail(call, "sceMpegAddStrCallback registers " + hex32(function) +
                       " for the video stream; the HLE replaces only " + hex32(config_.video_callback) +
                       ", so this video would be fed to the IPU");
    }
}

void SceMpegHle::hook_demux_pss_ring(OverrideCall& call) {
    Session& s = session(call, arg(call, 0), "sceMpegDemuxPssRing");
    s.ring_base = arg(call, 3);
    s.ring_bytes = arg(call, 4);
}

void SceMpegHle::hook_reset(OverrideCall& call) {
    Session& s = session(call, arg(call, 0), "sceMpegReset");
    s.model.reset();
    if (s.decoder) {
        s.decoder->reset();
    }
    s.flushed = false;
    s.end_reported = false;
}

void SceMpegHle::video_stream(OverrideCall& call) {
    const std::uint32_t mp = arg(call, 0);
    const std::uint32_t block = arg(call, 1);
    Session& s = session(call, mp, "the video stream callback");
    if (s.model.pending() >= config_.max_pending_pictures) {
        ++stats_.refusals;
        set_result(call, 0); // DemuxPssRing stops here and offers this packet again on its next call
        return;
    }
    GuestMemory& m = machine_->memory();
    const std::uint32_t data = m.load32(block + cb_data);
    const std::uint32_t length = m.load32(block + cb_length);
    const std::int64_t pts = load_s64(m, block + cb_pts);
    const std::int64_t dts = load_s64(m, block + cb_dts);

    // The payload may run past the demux ring's end and continue at its base.
    payload_.resize(length);
    std::uint32_t first = length;
    const std::uint64_t ring_end = std::uint64_t{s.ring_base} + s.ring_bytes;
    if (s.ring_bytes != 0 && data >= s.ring_base && data < ring_end &&
        data + std::uint64_t{length} > ring_end) {
        first = static_cast<std::uint32_t>(ring_end - data);
    }
    try {
        m.read_bytes(data, std::span<std::uint8_t>(payload_.data(), first));
        if (first < length) {
            m.read_bytes(s.ring_base, std::span<std::uint8_t>(payload_.data() + first, length - first));
        }
    } catch (const std::out_of_range& e) {
        fail(call, std::string("the video payload is not in RAM: ") + e.what());
    }
    s.model.feed(payload_, pts, dts);
    if (!s.model.error().empty()) {
        fail(call, "the video stream: " + s.model.error());
    }
    set_result(call, 1);
}

void SceMpegHle::is_end(OverrideCall& call) {
    Session& s = session(call, arg(call, 0), "sceMpegIsEnd");
    const bool end = s.model.finished();
    if (end && !s.end_reported) {
        s.end_reported = true;
        emit_movie("end", s);
    }
    set_result(call, end ? 1 : 0);
}

void SceMpegHle::decode_reached(OverrideCall& call) {
    fail(call, "the SDK's IPU picture decode was reached (called from " + hex32(gpr_u32(*call.ctx, 31)) +
                   "); the movie HLE replaces sceMpegGetPicture, so this path was not covered");
}

void SceMpegHle::get_picture(OverrideCall& call) {
    const std::uint32_t mp = arg(call, 0);
    const std::uint32_t destination = arg(call, 1);
    const std::uint32_t macroblocks = arg(call, 2);
    // The nodata callback's data block, below the caller's stack, allocated once for every call it gets.
    const std::uint32_t block = call.scratch(cb_block_bytes);
    // Nothing that must be undone lives across call_guest: the player may terminate this thread inside the
    // callback, and the session is looked up again after it returns.
    for (;;) {
        Session& s = session(call, mp, "sceMpegGetPicture");
        if (s.model.next_display() != nullptr) {
            deliver(call, s, mp, destination, macroblocks);
            set_result(call, 0);
            return;
        }
        if (s.model.finished()) {
            set_result(call, -1); // no picture left: the player should have asked sceMpegIsEnd
            return;
        }
        if (s.nodata == 0) {
            fail(call, "sceMpegGetPicture has no picture ready and no nodata callback to wait with");
        }
        const std::uint32_t function = s.nodata;
        const std::uint32_t user = s.nodata_data;
        GuestMemory& m = machine_->memory();
        for (std::uint32_t i = 0; i < cb_block_bytes; i += 4) {
            m.store32(block + i, 0);
        }
        m.store32(block + cb_type, callback_nodata);
        ++stats_.nodata_calls;
        const std::uint64_t r = call.call_guest(function, {mp, block, user});
        if (static_cast<std::uint32_t>(r) == 0) {
            set_result(call, -1); // the callback asked GetPicture to give up
            return;
        }
    }
}

const PictureYuv* SceMpegHle::paint(Session& s, const MpegDisplayPicture& want) {
    if (!s.decoder) {
        return nullptr;
    }
    try {
        for (;;) {
            PictureYuv p;
            if (s.decoder->next_picture(p)) {
                if (p.pts90k == std::int64_t{want.index}) {
                    picture_ = p;
                    return &picture_;
                }
                ++stats_.mismatched;
                continue;
            }
            if (auto coded = s.model.take_coded()) {
                s.decoder->decode_picture(coded->bytes, coded->index);
                continue;
            }
            if (s.model.ended() && !s.flushed) {
                s.flushed = true;
                s.decoder->flush();
                continue;
            }
            return nullptr;
        }
    } catch (const MovieDecoderError&) {
        return nullptr;
    }
}

void SceMpegHle::deliver(OverrideCall& call, Session& s, std::uint32_t mp, std::uint32_t destination,
                         std::uint32_t macroblocks) {
    const MpegDisplayPicture want = *s.model.next_display();
    const MpegSequenceInfo& seq = s.model.sequence();
    const PictureYuv* picture = paint(s, want);
    if (picture == nullptr || picture->width != seq.width || picture->height != seq.height) {
        ++stats_.decoder_errors;
        picture = &black_.get(seq.width, seq.height);
    }
    const IpuThresholds thresholds = ipu_ != nullptr ? ipu_->thresholds() : IpuThresholds{};
    convert_rgb32_macroblocks(*picture, macroblocks, thresholds, rgb32_);

    GuestMemory& m = machine_->memory();
    try {
        m.write_bytes(destination, rgb32_);
    } catch (const std::out_of_range& e) {
        fail(call, std::string("sceMpegGetPicture's destination is not in RAM: ") + e.what());
    }
    m.store32(mp + mp_width, seq.width);
    m.store32(mp + mp_height, seq.height);
    m.store32(mp + mp_frame, static_cast<std::uint32_t>(s.model.delivered()));
    store_s64(m, mp + mp_pts, want.pts90k);
    store_s64(m, mp + mp_dts, want.dts90k);

    s.model.pop_display();
    ++s.pictures;
    ++stats_.pictures;
    const std::uint32_t slot = destination & PS2_RAM_MASK;
    if (s.slot_a == 0) {
        s.slot_a = slot;
    } else if (s.slot_b == 0 && slot != s.slot_a) {
        s.slot_b = slot;
    }
    machine_->clock().charge(machine_->cost().movie_picture);
}

} // namespace dq8::rt
