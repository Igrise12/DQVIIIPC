# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session D: the host-folder memory card (io/memcard), the pad feedback and the virtual RTC. Joins
# dq8_runtime here.
target_sources(dq8_runtime PRIVATE
    io/memcard/mc_path.cpp
    io/memcard/memory_card.cpp
    io/memcard/folder_store.cpp
    machine/rtc.cpp
    io/pad_feedback.cpp
)
# The card folder's metadata files (Docs/formats/memory-card-folder.md).
target_link_libraries(dq8_runtime PRIVATE toml11::toml11)
