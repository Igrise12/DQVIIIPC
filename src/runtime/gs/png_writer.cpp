// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The one translation unit that compiles stb_image_write (third_party/stb, public domain / MIT).
#include "runtime/gs/gs_backends.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-align"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <cstddef>
#include <stb_image_write.h>
#include <string>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace dq8::gs {

bool write_png(const std::string& path, const FrameImage& frame) {
    if (frame.width == 0 || frame.height == 0 ||
        frame.rgba.size() < std::size_t{frame.width} * frame.height * 4u) {
        return false;
    }
    return stbi_write_png(path.c_str(), static_cast<int>(frame.width), static_cast<int>(frame.height), 4,
                          frame.rgba.data(), static_cast<int>(frame.width * 4)) != 0;
}

} // namespace dq8::gs
