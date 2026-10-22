# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session D: memory card, RTC, pad feedback, VU0 and kernel tests (test_memcard*, ...).
target_sources(dq8_unit_tests PRIVATE
    test_memcard.cpp
    test_pad_feedback.cpp
)
