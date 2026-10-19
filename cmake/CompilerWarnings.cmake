# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8_set_warnings(<target>): the project's warning set, applied to every first-party target.
function(dq8_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(DQ8_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
            -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
            -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
        if(DQ8_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
