# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8_set_warnings(<target>): the project's warning set, applied to every first-party target.
function(dq8_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        # std::getenv, fopen and friends are standard C++; MSVC's C4996 "unsafe" warnings would fail /WX.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(DQ8_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
            -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
        # -Wnull-dereference is deliberately absent: GCC 13 reports false positives inside libstdc++ and toml11
        # at -O3 (seen in CI), and with DQ8_WERROR those become errors we cannot fix.
        if(DQ8_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
