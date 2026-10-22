// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The launcher's status channel (status_channel.h).
#include "host/status_channel.h"

#include "dq8/core/error.h"
#include "dq8/core/version.h"
#include "host/options.h"
#include "host/run.h"
#include "host/session.h"

#include <cerrno>
#include <cstring>
#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dq8::host {

namespace {

long long process_id() {
#if defined(_WIN32)
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

// stdout for the status lines only: a duplicate of the original stdout, with stdout itself pointed at stderr.
std::FILE* take_stdout() {
    std::fflush(stdout);
#if defined(_WIN32)
    const int fd = _dup(_fileno(stdout));
    if (fd < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0) {
        return nullptr;
    }
    return _fdopen(fd, "w");
#else
    const int fd = dup(fileno(stdout));
    if (fd < 0 || dup2(fileno(stderr), fileno(stdout)) < 0) {
        return nullptr;
    }
    return fdopen(fd, "w");
#endif
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

} // namespace

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

StatusChannel::StatusChannel(const std::optional<std::string>& target) {
    if (!target) {
        return;
    }
    if (*target == "-") {
        out_ = take_stdout();
        if (out_ == nullptr) {
            throw core::Error(core::ErrorCode::io_write,
                              std::string("--status -: cannot take over stdout: ") + std::strerror(errno));
        }
    } else {
        out_ = std::fopen(target->c_str(), "w");
        if (out_ == nullptr) {
            throw core::Error(core::ErrorCode::io_write, "--status: cannot create '" + *target + "'");
        }
    }
    owns_file_ = true;
}

StatusChannel::~StatusChannel() {
    if (owns_file_ && out_ != nullptr) {
        std::fclose(out_);
    }
}

void StatusChannel::write_line(const std::string& line) {
    if (out_ == nullptr || exited_) {
        return;
    }
    std::fputs(line.c_str(), out_);
    std::fputc('\n', out_);
    std::fflush(out_);
}

void StatusChannel::started(std::string_view profile, bool window) {
    const std::lock_guard lock(mutex_);
    write_line(R"({"ev":"started","version":)" + json_string(core::version()) + R"(,"profile":)" +
               json_string(profile) + R"(,"pid":)" + std::to_string(process_id()) + R"(,"window":)" +
               (window ? "true" : "false") + "}");
}

void StatusChannel::frame() {
    if (out_ == nullptr) {
        return; // set once, in the constructor
    }
    const std::lock_guard lock(mutex_);
    ++frames_;
    if (!first_frame_sent_) {
        first_frame_sent_ = true;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count();
        write_line(R"({"ev":"first_frame","ms":)" + std::to_string(ms) + "}");
    }
}

void StatusChannel::guest_time(double seconds) {
    if (out_ == nullptr) {
        return;
    }
    const std::lock_guard lock(mutex_);
    const Clock::time_point now = Clock::now();
    if (!measuring_) {
        // The first slice starts the first second: start-up time is not frame time.
        measuring_ = true;
        window_start_ = now;
        window_frames_ = frames_;
        window_guest_ = seconds;
        return;
    }
    const double wall = std::chrono::duration<double>(now - window_start_).count();
    if (wall < 1.0) {
        return;
    }
    const double fps = static_cast<double>(frames_ - window_frames_) / wall;
    const double speed = (seconds - window_guest_) / wall;
    write_line(R"({"ev":"fps","fps":)" + number(fps) + R"(,"speed":)" + number(speed) + "}");
    window_start_ = now;
    window_frames_ = frames_;
    window_guest_ = seconds;
}

void StatusChannel::warning(std::string_view code, std::string_view message) {
    const std::lock_guard lock(mutex_);
    write_line(R"({"ev":"warning","code":)" + json_string(code) + R"(,"message":)" + json_string(message) +
               "}");
}

void StatusChannel::exit(int status, std::string_view reason, std::string_view code,
                         std::string_view message) {
    const std::lock_guard lock(mutex_);
    std::string line =
        R"({"ev":"exit","status":)" + std::to_string(status) + R"(,"reason":)" + json_string(reason);
    if (!code.empty()) {
        line += R"(,"code":)" + json_string(code);
    }
    if (!message.empty()) {
        line += R"(,"message":)" + json_string(message);
    }
    write_line(line + "}");
    exited_ = true;
}

void StatusChannel::exit_on(const std::exception_ptr& error) {
    // The same codes and statuses main() reports.
    try {
        std::rethrow_exception(error);
    } catch (const UsageError& e) {
        exit(host::exit_error, "error", "usage", e.what());
    } catch (const SetupProblems& e) {
        exit(host::exit_error, "error", "module-validation", e.what());
    } catch (const BackendError& e) {
        exit(exit_backend, "error", "backend", e.what());
    } catch (const core::Error& e) {
        exit(host::exit_error, "error", core::to_string(e.code()), e.what());
    } catch (const std::exception& e) {
        exit(host::exit_error, "error", "error", e.what());
    } catch (...) {
        exit(host::exit_error, "error", "error", "unknown error");
    }
}

} // namespace dq8::host
