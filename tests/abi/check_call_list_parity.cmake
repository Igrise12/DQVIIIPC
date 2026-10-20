# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# abi.call_list_parity: include/dq8/abi/ps2recomp/ps2_call_list.h must be byte-for-byte the pinned submodule's.
# Run as: cmake -DOURS=<file> -DUPSTREAM=<file> -P check_call_list_parity.cmake
foreach(_var OURS UPSTREAM)
    if(NOT EXISTS "${${_var}}")
        message(FATAL_ERROR "${_var} does not exist: '${${_var}}' (is third_party/PS2Recomp checked out? "
                            "git submodule update --init third_party/PS2Recomp)")
    endif()
endforeach()
file(SHA256 "${OURS}" _ours)
file(SHA256 "${UPSTREAM}" _upstream)
if(NOT _ours STREQUAL _upstream)
    message(FATAL_ERROR "ps2_call_list.h differs from the PS2Recomp submodule's copy.\n"
                        "  ours:     ${OURS}\n  upstream: ${UPSTREAM}\n"
                        "Copy the upstream file over ours (it is kept verbatim), then rebuild: the forwarders in "
                        "ps2_syscalls.h and ps2_stubs.h follow the list.")
endif()
message(STATUS "ps2_call_list.h matches the submodule (sha256 ${_ours})")
