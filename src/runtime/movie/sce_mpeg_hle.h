// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
#pragma once

#include "runtime/ee/overrides.h"
#include "runtime/machine/machine_services.h"
#include "runtime/movie/macroblock_writer.h"
#include "runtime/movie/movie_decoder.h"
#include "runtime/movie/mpeg_es_model.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The sceMpeg HLE (architecture §7.2, ADR-0015, Docs/milestones/M4-playable.md session C and its Protocol
// notes). The game keeps its own movie player: the reader thread, the XOR read, sceMpegDemuxPssRing, the PCM
// stream callback and the GIF upload stay recompiled game code. The HLE replaces only what would reach the
// IPU:
//   - the player's video stream callback: the demuxed video goes to an elementary-stream model (MpegEsModel)
//     instead of the IPU's DMA ring; it refuses a packet (returns 0, which makes DemuxPssRing stop and offer
//     the packet again later) while `max_pending_pictures` complete pictures wait;
//   - sceMpegGetPicture: the next picture in display order, painted by an IMovieDecoder (black when none
//   can),
//     converted by the IPU's CSC and written as RGB32 macroblocks; while no picture is ready it calls the
//     player's nodata callback, which yields to the reader thread;
//   - sceMpegIsEnd: true once the model has delivered the last picture.
// It hooks sceMpegCreate, sceMpegDelete, AddCallback, AddStrCallback, DemuxPssRing and Reset to follow the
// sessions (keyed by the sceMpeg structure's address, `mp`) and stops on the SDK's own IPU decode. Only SDK
// knowledge lives here: the entry addresses come from the region profile through src/game/movies.cpp.
//
// Determinism: the model alone decides which picture is delivered when; each delivered picture costs
// CostModel::movie_picture; a decoder only chooses pixels. GetPicture keeps no host state across the nodata
// callback, because the stop routine of a player may terminate the thread inside it.
namespace dq8::rt {

class IpuRegisters;

struct SceMpegHleConfig {
    // One decoder per session (sceMpegCreate); nodecode makes blank ones. Throws MovieDecoderError when it
    // cannot (the HLE then stops: the policy promised pictures).
    std::function<std::unique_ptr<IMovieDecoder>()> make_decoder;
    // The video stream callback the HLE replaces. AddStrCallback must register this function for the video
    // stream; any other would send video to the IPU.
    std::uint32_t video_callback = 0;
    // Complete pictures the model holds before the video callback refuses data.
    std::uint32_t max_pending_pictures = 4;
};

class SceMpegHle final : public IDevice {
  public:
    // The SDK's numbers: sceMpegAddCallback types, the type word of a callback's data block, stream types.
    static constexpr std::uint32_t callback_nodata = 1;
    static constexpr std::uint32_t stream_video = 0;

    explicit SceMpegHle(SceMpegHleConfig config);
    ~SceMpegHle() override;
    SceMpegHle(const SceMpegHle&) = delete;
    SceMpegHle& operator=(const SceMpegHle&) = delete;

    std::string_view name() const override { return "sce_mpeg_hle"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    // Override bodies. hook_* run before the guest function (OverrideKind::hook_before); the others replace
    // it.
    void hook_create(OverrideCall& call);       // sceMpegCreate(mp, work, size)
    void hook_delete(OverrideCall& call);       // sceMpegDelete(mp)
    void hook_add_callback(OverrideCall& call); // sceMpegAddCallback(mp, type, function, data)
    void
    hook_add_str_callback(OverrideCall& call);    // sceMpegAddStrCallback(mp, stream type, channel, fn, data)
    void hook_demux_pss_ring(OverrideCall& call); // sceMpegDemuxPssRing(mp, start, bytes, ring, ring bytes)
    void hook_reset(OverrideCall& call);          // sceMpegReset(mp)
    // The game's player open: argument `name_argument` (0 = $a0) is the movie's name, a C string; the next
    // sceMpegCreate takes it. Emits `movie` phase "open".
    void hook_player_open(OverrideCall& call, unsigned name_argument);
    void get_picture(OverrideCall& call);    // sceMpegGetPicture(mp, destination, macroblocks)
    void is_end(OverrideCall& call);         // sceMpegIsEnd(mp)
    void video_stream(OverrideCall& call);   // the video stream callback (mp, callback data, user data)
    void decode_reached(OverrideCall& call); // the SDK's IPU picture decode: stops

    struct Stats {
        std::uint64_t sessions = 0;
        std::uint64_t pictures = 0;
        std::uint64_t refusals = 0; // video packets refused (backpressure)
        std::uint64_t nodata_calls = 0;
        std::uint64_t decoder_errors = 0; // pictures painted black because the decoder failed
        std::uint64_t mismatched = 0;     // decoder pictures that were not the one the model asked for
    };
    const Stats& stats() const noexcept { return stats_; }
    // Sessions open now (for tests).
    std::size_t open_sessions() const noexcept { return sessions_.size(); }

  private:
    struct Session {
        std::string name;
        MpegEsModel model;
        std::unique_ptr<IMovieDecoder> decoder;
        bool flushed = false; // the decoder was told the stream ended
        std::uint32_t nodata = 0;
        std::uint32_t nodata_data = 0;
        std::uint32_t ring_base = 0; // the demux ring of the DemuxPssRing call in progress (0: unknown)
        std::uint32_t ring_bytes = 0;
        std::uint32_t slot_a = 0; // the first two GetPicture destinations (physical)
        std::uint32_t slot_b = 0;
        std::uint64_t pictures = 0; // delivered over the session's life
        bool end_reported = false;
    };

    Session& session(const OverrideCall& call, std::uint32_t mp, std::string_view what);
    void deliver(OverrideCall& call, Session& s, std::uint32_t mp, std::uint32_t destination,
                 std::uint32_t macroblocks);
    const PictureYuv* paint(Session& s, const MpegDisplayPicture& want);
    void emit_movie(std::string_view phase, const Session& s);
    [[noreturn]] void fail(const OverrideCall& call, std::string message);

    SceMpegHleConfig config_;
    MachineServices* machine_ = nullptr;
    IpuRegisters* ipu_ = nullptr; // SETTH thresholds; null when no IPU stub is attached
    std::map<std::uint32_t, Session> sessions_;
    std::string next_name_;
    Stats stats_;
    std::vector<std::uint8_t> payload_; // scratch: one video packet
    std::vector<std::uint8_t> rgb32_;   // scratch: one converted picture
    PictureYuv picture_;
    BlackPicture black_;
};

} // namespace dq8::rt
