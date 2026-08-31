# Optional diagnostics for AYTHER-owned native targets. Third-party targets are
# intentionally left untouched so reports stay attributable to this project.

function(ayther_configure_instrumentation)
    add_library(ayther_instrumentation INTERFACE)
    add_library(ayther_strict_warnings INTERFACE)

    if(MSVC)
        target_compile_options(ayther_strict_warnings INTERFACE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(ayther_strict_warnings INTERFACE
            -Wall -Wextra -Wpedantic -Werror)
    else()
        message(FATAL_ERROR
            "AYTHER warnings-as-errors require Clang, GCC, or MSVC; got "
            "${CMAKE_CXX_COMPILER_ID}.")
    endif()

    if(AYTHER_ENABLE_ASAN OR AYTHER_ENABLE_UBSAN)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            message(FATAL_ERROR
                "AYTHER sanitizers require Clang or GCC; got ${CMAKE_CXX_COMPILER_ID}.")
        endif()

        set(_ayther_sanitizers "")
        if(AYTHER_ENABLE_ASAN)
            list(APPEND _ayther_sanitizers address)
        endif()
        if(AYTHER_ENABLE_UBSAN)
            list(APPEND _ayther_sanitizers undefined)
        endif()
        list(JOIN _ayther_sanitizers "," _ayther_sanitizer_flags)

        target_compile_options(ayther_instrumentation INTERFACE
            "-fsanitize=${_ayther_sanitizer_flags}"
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(ayther_instrumentation INTERFACE
            "-fsanitize=${_ayther_sanitizer_flags}"
            -fno-sanitize-recover=all)
    endif()

    if(AYTHER_ENABLE_COVERAGE)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            message(FATAL_ERROR
                "AYTHER native coverage requires Clang or GCC; got ${CMAKE_CXX_COMPILER_ID}.")
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # The instrumentation flags are the same either way, but the
            # optimisation and debug switches are not: clang-cl takes the MSVC
            # spellings and REJECTS -O0/-g as unused arguments, which under
            # warnings-as-errors fails the build rather than being ignored.
            target_compile_options(ayther_instrumentation INTERFACE
                -fprofile-instr-generate -fcoverage-mapping)
            if(MSVC)
                target_compile_options(ayther_instrumentation INTERFACE /Od /Zi)
            else()
                target_compile_options(ayther_instrumentation INTERFACE -O0 -g)
            endif()
            target_link_options(ayther_instrumentation INTERFACE
                -fprofile-instr-generate -fcoverage-mapping)
        else()
            target_compile_options(ayther_instrumentation INTERFACE
                --coverage -O0 -g)
            target_link_options(ayther_instrumentation INTERFACE --coverage)
        endif()
    endif()
endfunction()

function(ayther_instrument_target target_name)
    cmake_parse_arguments(ARG "NO_STRICT_WARNINGS" "" "" ${ARGN})
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "Cannot instrument missing target: ${target_name}")
    endif()
    target_link_libraries("${target_name}" PRIVATE
        $<BUILD_INTERFACE:ayther_instrumentation>)
    if(NOT ARG_NO_STRICT_WARNINGS)
        target_link_libraries("${target_name}" PRIVATE
            $<BUILD_INTERFACE:ayther_strict_warnings>)
    endif()
endfunction()
