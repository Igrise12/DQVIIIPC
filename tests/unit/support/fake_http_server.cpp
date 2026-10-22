// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "support/fake_http_server.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t bad_socket = INVALID_SOCKET;
void close_socket(socket_t s) {
    closesocket(s);
}
constexpr int send_flags = 0;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t bad_socket = -1;
void close_socket(socket_t s) {
    ::close(s);
}
#if defined(MSG_NOSIGNAL)
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif
#endif

namespace dq8::test {

namespace {

socket_t to_socket(std::intptr_t s) {
    return static_cast<socket_t>(s);
}

// Waits up to 50 ms for `s` to become readable.
bool readable(socket_t s) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval wait{0, 50000};
#if defined(_WIN32)
    return select(0, &set, nullptr, nullptr, &wait) > 0;
#else
    return select(s + 1, &set, nullptr, nullptr, &wait) > 0;
#endif
}

bool send_all(socket_t s, const char* data, std::size_t size) {
    while (size > 0) {
        const std::size_t chunk = std::min<std::size_t>(size, 16384);
#if defined(_WIN32)
        const int sent = ::send(s, data, static_cast<int>(chunk), send_flags);
#else
        const ssize_t sent = ::send(s, data, chunk, send_flags);
#endif
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool send_all(socket_t s, std::string_view text) {
    return send_all(s, text.data(), text.size());
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return s;
}

} // namespace

FakeHttpServer::FakeHttpServer(std::string path, std::string body, Behaviour behaviour)
    : path_(std::move(path)), body_(std::move(body)), behaviour_(std::move(behaviour)) {
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("fake http server: WSAStartup failed");
    }
#endif
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == bad_socket) {
        throw std::runtime_error("fake http server: socket failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof address;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 || ::listen(s, 8) != 0 ||
        ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close_socket(s);
        throw std::runtime_error("fake http server: cannot listen on 127.0.0.1");
    }
    listener_ = static_cast<std::intptr_t>(s);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { serve(); });
}

FakeHttpServer::~FakeHttpServer() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    close_socket(to_socket(listener_));
#if defined(_WIN32)
    WSACleanup();
#endif
}

std::string FakeHttpServer::url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
}

std::vector<FakeHttpServer::Request> FakeHttpServer::requests() const {
    const std::lock_guard lock(mutex_);
    return requests_;
}

void FakeHttpServer::serve() {
    const socket_t listener = to_socket(listener_);
    while (!stop_) {
        if (!readable(listener)) {
            continue;
        }
        const socket_t client = ::accept(listener, nullptr, nullptr);
        if (client == bad_socket) {
            continue;
        }
        answer(static_cast<std::intptr_t>(client));
        close_socket(client);
    }
}

void FakeHttpServer::answer(std::intptr_t handle) {
    const socket_t client = to_socket(handle);
    std::string head;
    char buffer[4096];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 65536) {
        if (stop_) {
            return;
        }
        if (!readable(client)) {
            continue;
        }
#if defined(_WIN32)
        const int got = ::recv(client, buffer, static_cast<int>(sizeof buffer), 0);
#else
        const ssize_t got = ::recv(client, buffer, sizeof buffer, 0);
#endif
        if (got <= 0) {
            return;
        }
        head.append(buffer, static_cast<std::size_t>(got));
    }

    Request request;
    const std::size_t first_space = head.find(' ');
    const std::size_t second_space = head.find(' ', first_space + 1);
    if (first_space != std::string::npos && second_space != std::string::npos) {
        request.path = head.substr(first_space + 1, second_space - first_space - 1);
    }
    const std::string lowered = lower(head);
    if (const std::size_t at = lowered.find("\r\nrange: bytes="); at != std::string::npos) {
        request.range_start = std::stoll(head.substr(at + 15));
    }

    std::string response;
    std::uint64_t start = 0;
    bool send_body = false;
    if (behaviour_.status != 0) {
        request.status = behaviour_.status;
        response = "HTTP/1.1 " + std::to_string(behaviour_.status) + " Fake\r\nContent-Length: 0\r\n";
    } else if (!behaviour_.redirect_from.empty() && request.path == behaviour_.redirect_from) {
        request.status = 302;
        response = "HTTP/1.1 302 Found\r\nLocation: " + url() + "\r\nContent-Length: 0\r\n";
    } else if (request.path != path_) {
        request.status = 404;
        response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n";
    } else if (request.range_start >= 0 && !behaviour_.ignore_range) {
        start = static_cast<std::uint64_t>(request.range_start);
        if (start >= body_.size()) {
            request.status = 416;
            response = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                       std::to_string(body_.size()) + "\r\nContent-Length: 0\r\n";
        } else {
            request.status = 206;
            response = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) + "-" +
                       std::to_string(body_.size() - 1) + "/" + std::to_string(body_.size()) +
                       "\r\nContent-Length: " + std::to_string(body_.size() - start) + "\r\n";
            send_body = true;
        }
    } else {
        request.status = 200;
        response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_.size()) + "\r\n";
        send_body = true;
    }
    response += "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
    {
        const std::lock_guard lock(mutex_);
        requests_.push_back(request);
    }
    if (!send_all(client, response) || !send_body) {
        return;
    }
    std::uint64_t size = body_.size() - start;
    if (behaviour_.drop_after != 0 && !dropped_) {
        dropped_ = true;
        size = std::min(size, behaviour_.drop_after);
    }
    send_all(client, body_.data() + start, static_cast<std::size_t>(size));
}

} // namespace dq8::test
