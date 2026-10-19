# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8_enable_sanitizers(<target>): honours DQ8_SANITIZERS (e.g. "address,undefined") on GCC and Clang.
function(dq8_enable_sanitizers target)
    if(DQ8_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=${DQ8_SANITIZERS} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${DQ8_SANITIZERS})
    endif()
endfunction()
