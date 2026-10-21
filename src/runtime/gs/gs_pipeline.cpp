// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/gs/gs_pipeline.h"

#include "dq8/core/hash.h"
#include "runtime/gs/gs_backends.h"
#include "runtime/gs/gs_trace_format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dq8::gs {

namespace {

struct ReadRequest {
    std::span<std::uint8_t> out;
    std::size_t result = 0;
    bool done = false;
};

struct Command {
    enum class Kind : std::uint8_t { transfer, vsync, read, dump };
    Kind kind = Kind::transfer;
    GifPath path = GifPath::path3;
    std::vector<std::uint8_t> bytes;
    std::unique_ptr<VsyncInfo> vsync;
    std::uint64_t field = 0;
    std::string file;
    ReadRequest* read = nullptr;

    std::size_t cost() const noexcept { return bytes.size() + (vsync ? sizeof(VsyncInfo) : 0) + 64; }
};

std::string hex(const std::array<std::uint8_t, 20>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (const std::uint8_t b : digest) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

} // namespace

struct GsPipeline::Impl {
    std::unique_ptr<IGsBackend> backend;
    GsCaps caps;
    bool inline_mode = false;
    std::size_t limit = 0;
    std::size_t flush_at = 0;

    // Producer (machine thread).
    std::vector<Command> batch;
    std::size_t batch_bytes = 0;
    core::Sha1 chain;
    std::vector<std::uint8_t> record; // scratch for record headers

    // Shared between the producer and the render thread.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<Command>> queue;
    std::size_t queued_bytes = 0;
    bool stopping = false;
    std::exception_ptr error;
    std::thread render;

    // Hash and counters, readable from any thread.
    mutable std::mutex hash_mutex;
    std::array<std::uint8_t, 20> last{};
    std::atomic<std::uint64_t> vsyncs{0};

    // Frame mailbox.
    mutable std::mutex frame_mutex;
    FrameImage latest;
    bool have_frame = false;

    // Render thread only (the calling thread in inline mode).
    std::map<std::uint64_t, std::vector<std::string>> dumps;

    void hash(std::span<const std::uint8_t> bytes) { chain.update(bytes); }

    void execute(Command& c) {
        switch (c.kind) {
        case Command::Kind::transfer:
            backend->gif_transfer(c.path, c.bytes);
            break;
        case Command::Kind::vsync:
            render_vsync(*c.vsync);
            break;
        case Command::Kind::read:
            c.read->result = backend->read_transfer(c.read->out);
            break;
        case Command::Kind::dump:
            dumps[c.field].push_back(std::move(c.file));
            break;
        }
    }

    void render_vsync(const VsyncInfo& info) {
        backend->vsync(info);
        FrameImage frame;
        bool have = false;
        if (caps.produces_pixels && backend->read_frame(frame)) {
            frame.field = info.field;
            have = true;
        }
        if (const auto it = dumps.find(info.field); it != dumps.end() && have) {
            for (const std::string& file : it->second) {
                write_png(file, frame);
            }
        }
        dumps.erase(dumps.begin(), dumps.upper_bound(info.field));
        if (have) {
            const std::lock_guard lock(frame_mutex);
            latest = std::move(frame);
            have_frame = true;
        }
    }

    void render_loop() {
        for (;;) {
            std::vector<Command> work;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    break; // stopping and drained
                }
                work = std::move(queue.front());
                queue.pop_front();
                for (const Command& c : work) {
                    queued_bytes -= c.cost();
                }
            }
            cv.notify_all();
            for (Command& c : work) {
                bool failed = false;
                {
                    const std::lock_guard lock(mutex);
                    failed = error != nullptr;
                }
                if (!failed) {
                    try {
                        execute(c);
                    } catch (...) {
                        const std::lock_guard lock(mutex);
                        error = std::current_exception();
                    }
                }
                if (c.kind == Command::Kind::read) {
                    {
                        const std::lock_guard lock(mutex);
                        c.read->done = true;
                    }
                    cv.notify_all();
                }
            }
        }
    }

    void push(Command c) {
        batch_bytes += c.cost();
        batch.push_back(std::move(c));
        if (batch_bytes >= flush_at) {
            flush();
        }
    }

    // Hands the batch to the render thread, waiting (in wall time only) while the queue is full.
    void flush() {
        if (batch.empty()) {
            return;
        }
        std::unique_lock lock(mutex);
        cv.wait(lock,
                [&] { return queue.empty() || queued_bytes + batch_bytes <= limit || error != nullptr; });
        if (error != nullptr) {
            batch.clear(); // nothing more reaches a failed backend, and no read request is left dangling
            batch_bytes = 0;
            std::rethrow_exception(error);
        }
        queued_bytes += batch_bytes;
        queue.push_back(std::move(batch));
        batch.clear();
        batch_bytes = 0;
        lock.unlock();
        cv.notify_all();
    }
};

GsPipeline::GsPipeline(GsPipelineConfig config) : impl_(std::make_unique<Impl>()) {
    if (!config.backend) {
        throw std::invalid_argument("GsPipeline: no backend");
    }
    impl_->backend = std::move(config.backend);
    impl_->caps = impl_->backend->caps();
    impl_->inline_mode = config.inline_mode;
    impl_->limit = std::max<std::size_t>(config.queue_bytes, std::size_t{1} << 20);
    impl_->flush_at = std::min<std::size_t>(impl_->limit / 8, std::size_t{1} << 20);
    impl_->chain.update(impl_->last); // h_0 = 20 zero bytes
    if (!impl_->inline_mode) {
        impl_->render = std::thread([impl = impl_.get()] { impl->render_loop(); });
    }
}

GsPipeline::~GsPipeline() {
    if (impl_->inline_mode) {
        return;
    }
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->batch.empty()) {
            impl_->queued_bytes += impl_->batch_bytes;
            impl_->queue.push_back(std::move(impl_->batch));
        }
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    impl_->render.join();
}

void GsPipeline::gif_transfer(GifPath path, std::span<const std::uint8_t> qwords) {
    Impl& p = *impl_;
    p.record.clear();
    trace::transfer_header(p.record, path, qwords.size());
    p.hash(p.record);
    p.hash(qwords);
    if (p.inline_mode) {
        p.backend->gif_transfer(path, qwords);
        return;
    }
    Command c;
    c.kind = Command::Kind::transfer;
    c.path = path;
    c.bytes.assign(qwords.begin(), qwords.end());
    p.push(std::move(c));
}

void GsPipeline::vsync(const VsyncInfo& info) {
    Impl& p = *impl_;
    p.record.clear();
    trace::vsync_records(p.record, info);
    p.hash(p.record);
    {
        const std::lock_guard lock(p.hash_mutex);
        p.last = p.chain.finish();
        p.chain.reset();
        p.chain.update(p.last);
    }
    p.vsyncs.fetch_add(1, std::memory_order_relaxed);
    if (p.inline_mode) {
        p.render_vsync(info);
        return;
    }
    Command c;
    c.kind = Command::Kind::vsync;
    c.vsync = std::make_unique<VsyncInfo>(info);
    p.push(std::move(c));
    p.flush();
}

std::size_t GsPipeline::read_transfer(std::span<std::uint8_t> out) {
    Impl& p = *impl_;
    p.record.clear();
    trace::read_fifo_record(p.record, out.size());
    p.hash(p.record);
    if (p.inline_mode) {
        return p.backend->read_transfer(out);
    }
    ReadRequest request{out};
    Command c;
    c.kind = Command::Kind::read;
    c.read = &request;
    p.push(std::move(c));
    p.flush();
    std::unique_lock lock(p.mutex);
    p.cv.wait(lock, [&] { return request.done; });
    if (p.error != nullptr) {
        std::rethrow_exception(p.error);
    }
    return request.result;
}

std::string GsPipeline::stream_hash() const {
    const std::lock_guard lock(impl_->hash_mutex);
    return hex(impl_->last);
}

std::uint64_t GsPipeline::vsyncs() const noexcept {
    return impl_->vsyncs.load(std::memory_order_relaxed);
}

bool GsPipeline::latest_frame(FrameImage& out) const {
    const std::lock_guard lock(impl_->frame_mutex);
    if (!impl_->have_frame) {
        return false;
    }
    out = impl_->latest;
    return true;
}

void GsPipeline::request_frame_dump(std::uint64_t field, std::string path) {
    Impl& p = *impl_;
    if (p.inline_mode) {
        p.dumps[field].push_back(std::move(path));
        return;
    }
    Command c;
    c.kind = Command::Kind::dump;
    c.field = field;
    c.file = std::move(path);
    p.push(std::move(c));
}

const GsCaps& GsPipeline::caps() const noexcept {
    return impl_->caps;
}

} // namespace dq8::gs
