// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/rtc.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace dq8::rt {

namespace {

constexpr std::int64_t seconds_per_day = 86'400;

bool leap_year(int y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int days_in_month(int y, int m) noexcept {
    constexpr int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap_year(y) ? 29 : days[m - 1];
}

// Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar (the usual era-based conversion).
std::int64_t days_from_civil(int y, int m, int d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return std::int64_t{era} * 146'097 + doe - 719'468;
}

void civil_from_days(std::int64_t z, int& y, int& m, int& d) noexcept {
    z += 719'468;
    const std::int64_t era = (z >= 0 ? z : z - 146'096) / 146'097;
    const auto doe = static_cast<int>(z - era * 146'097);
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146'096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yoe + era * 400) + (m <= 2 ? 1 : 0);
}

const std::int64_t days_to_2000 = days_from_civil(2000, 1, 1);

} // namespace

std::string format_rtc(const RtcDateTime& t) {
    char text[32];
    std::snprintf(text, sizeof text, "%04u-%02u-%02uT%02u:%02u:%02u", unsigned{t.year}, unsigned{t.month},
                  unsigned{t.day}, unsigned{t.hour}, unsigned{t.minute}, unsigned{t.second});
    return text;
}

std::optional<RtcDateTime> parse_rtc(std::string_view v) {
    if (v.size() != 19 || v[4] != '-' || v[7] != '-' || v[10] != 'T' || v[13] != ':' || v[16] != ':') {
        return std::nullopt;
    }
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
        if (v[i] < '0' || v[i] > '9') {
            return std::nullopt;
        }
    }
    const auto num = [&](std::size_t at, std::size_t n) {
        int x = 0;
        for (std::size_t i = 0; i < n; ++i) {
            x = x * 10 + (v[at + i] - '0');
        }
        return x;
    };
    const int year = num(0, 4);
    const int month = num(5, 2);
    const int day = num(8, 2);
    const int hour = num(11, 2);
    const int minute = num(14, 2);
    const int second = num(17, 2);
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    return RtcDateTime{static_cast<std::uint16_t>(year),  static_cast<std::uint8_t>(month),
                       static_cast<std::uint8_t>(day),    static_cast<std::uint8_t>(hour),
                       static_cast<std::uint8_t>(minute), static_cast<std::uint8_t>(second)};
}

std::int64_t rtc_seconds(const RtcDateTime& t) noexcept {
    const std::int64_t days = days_from_civil(t.year, t.month, t.day) - days_to_2000;
    return days * seconds_per_day + std::int64_t{t.hour} * 3600 + std::int64_t{t.minute} * 60 + t.second;
}

RtcDateTime rtc_from_seconds(std::int64_t seconds) noexcept {
    // The PS2's clock keeps two-digit years: clamp to 2000..2099 rather than wrap.
    const std::int64_t max = rtc_seconds(RtcDateTime{2099, 12, 31, 23, 59, 59});
    seconds = std::clamp<std::int64_t>(seconds, 0, max);
    const std::int64_t days = seconds / seconds_per_day;
    const std::int64_t rest = seconds % seconds_per_day;
    int y = 0;
    int m = 0;
    int d = 0;
    civil_from_days(days + days_to_2000, y, m, d);
    return RtcDateTime{static_cast<std::uint16_t>(y),
                       static_cast<std::uint8_t>(m),
                       static_cast<std::uint8_t>(d),
                       static_cast<std::uint8_t>(rest / 3600),
                       static_cast<std::uint8_t>(rest / 60 % 60),
                       static_cast<std::uint8_t>(rest % 60)};
}

RtcDateTime VirtualRtc::default_epoch() noexcept {
    return RtcDateTime{2005, 11, 15, 12, 0, 0};
}

RtcDateTime VirtualRtc::host_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const int year = std::clamp(tm.tm_year + 1900, 2000, 2099);
    return RtcDateTime{
        static_cast<std::uint16_t>(year),      static_cast<std::uint8_t>(tm.tm_mon + 1),
        static_cast<std::uint8_t>(tm.tm_mday), static_cast<std::uint8_t>(tm.tm_hour),
        static_cast<std::uint8_t>(tm.tm_min),  static_cast<std::uint8_t>(std::min(tm.tm_sec, 59))};
}

RtcDateTime VirtualRtc::at(Cycles t) const noexcept {
    return rtc_from_seconds(rtc_seconds(epoch_) + static_cast<std::int64_t>(t / ee_clock_hz));
}

} // namespace dq8::rt
