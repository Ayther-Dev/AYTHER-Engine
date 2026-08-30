# Optional diagnostics for AYTHER-owned native targets. Third-party targets are
# intentionally left untouched so reports stay attributable to this project.

function(ayther_configure_instrumentation)
    add_library(ayther_instrumentation INTERFACE)

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
        target_compile_options(ayther_instrumentation INTERFACE
            --coverage -O0 -g)
        target_link_options(ayther_instrumentation INTERFACE --coverage)
    endif()
endfunction()

function(ayther_instrument_target target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "Cannot instrument missing target: ${target_name}")
    endif()
    target_link_libraries("${target_name}" PRIVATE
        $<BUILD_INTERFACE:ayther_instrumentation>)
endfunction()
