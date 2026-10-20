// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Minimal JSON writer for dq8-game's --json output: objects, arrays, strings, integers, booleans, null.
// The same shape as dq8ctl's writer (src/cli/main.cpp); both should move to dq8::core when a third user
// appears.
namespace dq8::host {

class JsonWriter {
  public:
    void begin_object() {
        separate();
        out_ += '{';
        first_.push_back(true);
    }
    void end_object() {
        out_ += '}';
        first_.pop_back();
    }
    void begin_array() {
        separate();
        out_ += '[';
        first_.push_back(true);
    }
    void end_array() {
        out_ += ']';
        first_.pop_back();
    }
    void key(std::string_view k) {
        separate();
        quote(k);
        out_ += ':';
        pending_key_ = true;
    }
    void value(std::string_view s) {
        separate();
        quote(s);
    }
    void value(const char* s) { value(std::string_view(s)); }
    void value(const std::string& s) { value(std::string_view(s)); }
    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    void value(T n) {
        separate();
        out_ += std::to_string(n);
    }
    void value(bool b) {
        separate();
        out_ += b ? "true" : "false";
    }
    void null() {
        separate();
        out_ += "null";
    }
    template <typename T> void member(std::string_view k, const T& v) {
        key(k);
        value(v);
    }
    const std::string& str() const { return out_; }

  private:
    void separate() {
        if (pending_key_) {
            pending_key_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ += ',';
            }
            first_.back() = false;
        }
    }
    void quote(std::string_view s) {
        out_ += '"';
        for (const char c : s) {
            switch (c) {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out_ += buf;
                } else {
                    out_ += c;
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool pending_key_ = false;
};

} // namespace dq8::host
