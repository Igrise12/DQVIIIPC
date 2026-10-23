// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A small HTTP/1.1 server on 127.0.0.1 for the toolchain download tests (test_toolchain_download.cpp): one
// body at one path, "Range: bytes=N-" honoured, and misbehaviour on request. One connection at a time,
// "Connection: close" on every response. Winsock on Windows, BSD sockets elsewhere.
namespace dq8::test {

class FakeHttpServer {
  public:
    struct Behaviour {
        std::uint64_t drop_after = 0; // the first body response closes the connection after this many bytes
        bool ignore_range = false;    // answer 200 with the whole body even when a range is asked for
        int status = 0;               // non-zero: answer this status with an empty body (404, 500, ...)
        std::string redirect_from;    // a second path answering 302 with the body's URL
    };

    struct Request {
        std::string path;
        std::int64_t range_start = -1; // -1: no Range header
        int status = 0;                // what the server answered
    };

    FakeHttpServer(std::string path, std::string body, Behaviour behaviour);
    FakeHttpServer(std::string path, std::string body)
        : FakeHttpServer(std::move(path), std::move(body), {}) {}
    ~FakeHttpServer();
    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    // "http://127.0.0.1:<port><path>"
    std::string url(const std::string& path) const;
    std::string url() const { return url(path_); }
    std::vector<Request> requests() const;

  private:
    void serve();
    void answer(std::intptr_t client);

    std::string path_;
    std::string body_;
    Behaviour behaviour_;
    std::intptr_t listener_ = -1;
    int port_ = 0;
    bool dropped_ = false;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::vector<Request> requests_;
    std::thread thread_;
};

} // namespace dq8::test
