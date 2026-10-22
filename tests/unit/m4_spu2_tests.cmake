# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session B: the SPU2 and audio output tests (test_spu2_*, test_audio_*).
target_sources(dq8_unit_tests PRIVATE
    test_audio_output.cpp
    test_spu2_adpcm.cpp
    test_spu2_chip.cpp
    test_spu2_device.cpp
)
