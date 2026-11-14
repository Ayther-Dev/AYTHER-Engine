# The installed core probe (TP-013.5) over the published vectors (TP-013.6).
#
#   cmake -DAYTHER_PACKAGE_PREFIX=<install prefix> -P check_core_probe.cmake
#
# Also included by CMakeLists.txt at configure time of the package consumer.
if(NOT DEFINED AYTHER_PACKAGE_PREFIX OR NOT IS_DIRECTORY "${AYTHER_PACKAGE_PREFIX}")
    message(FATAL_ERROR "Pass -DAYTHER_PACKAGE_PREFIX=<install prefix>")
endif()
if(NOT DEFINED CMAKE_EXECUTABLE_SUFFIX OR CMAKE_EXECUTABLE_SUFFIX STREQUAL "")
    if(CMAKE_HOST_WIN32)
        set(CMAKE_EXECUTABLE_SUFFIX ".exe")
    endif()
endif()
if(NOT DEFINED CMAKE_SHARED_MODULE_SUFFIX OR CMAKE_SHARED_MODULE_SUFFIX STREQUAL "")
    if(CMAKE_HOST_WIN32)
        set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
    else()
        set(CMAKE_SHARED_MODULE_SUFFIX ".so")
    endif()
endif()

set(_ayther_probe_tool
    "${AYTHER_PACKAGE_PREFIX}/bin/ayther_core_probe${CMAKE_EXECUTABLE_SUFFIX}")
set(_ayther_probe_vectors_dir "${AYTHER_PACKAGE_PREFIX}/share/ayther/probe-vectors")
if(NOT EXISTS "${_ayther_probe_tool}")
    message(FATAL_ERROR "The installed core probe is missing: ${_ayther_probe_tool}")
endif()
if(NOT EXISTS "${_ayther_probe_vectors_dir}/probe-vectors.json")
    message(FATAL_ERROR
        "The installed probe vectors are missing: ${_ayther_probe_vectors_dir}")
endif()

# A command-line usage error is exit 1 and prints no status line.
execute_process(COMMAND "${_ayther_probe_tool}"
    RESULT_VARIABLE _ayther_probe_usage_result
    OUTPUT_VARIABLE _ayther_probe_usage_output
    ERROR_QUIET)
if(NOT _ayther_probe_usage_result EQUAL 1 OR
   _ayther_probe_usage_output MATCHES "AYTHER_STATUS")
    message(FATAL_ERROR
        "ayther_core_probe without arguments must exit 1 without a status "
        "line, got ${_ayther_probe_usage_result}")
endif()

file(READ "${_ayther_probe_vectors_dir}/probe-vectors.json" _ayther_vectors_json)
string(JSON _ayther_vector_count LENGTH "${_ayther_vectors_json}" vectors)
if(NOT _ayther_vector_count EQUAL 8)
    message(FATAL_ERROR
        "The contract has eight vectors, the file lists ${_ayther_vector_count}")
endif()
string(JSON _ayther_vector_limit GET "${_ayther_vectors_json}" time_limit_seconds)
math(EXPR _ayther_vector_last "${_ayther_vector_count} - 1")
set(_ayther_vectors_run 0)
foreach(_ayther_index RANGE 0 ${_ayther_vector_last})
    string(JSON _ayther_id GET "${_ayther_vectors_json}" vectors ${_ayther_index} id)
    string(JSON _ayther_external ERROR_VARIABLE _ayther_no_external
        GET "${_ayther_vectors_json}" vectors ${_ayther_index} external)
    if(_ayther_no_external STREQUAL "NOTFOUND")
        continue()
    endif()

    string(JSON _ayther_library ERROR_VARIABLE _ayther_no_library
        GET "${_ayther_vectors_json}" vectors ${_ayther_index} library)
    if(_ayther_no_library STREQUAL "NOTFOUND")
        set(_ayther_vector_file
            "${_ayther_probe_vectors_dir}/${_ayther_library}${CMAKE_SHARED_MODULE_SUFFIX}")
    else()
        string(JSON _ayther_file GET "${_ayther_vectors_json}" vectors ${_ayther_index} file)
        set(_ayther_vector_file "${_ayther_probe_vectors_dir}/${_ayther_file}")
    endif()
    string(JSON _ayther_expected GET "${_ayther_vectors_json}" vectors ${_ayther_index} expect)
    string(JSON _ayther_verdict GET "${_ayther_expected}" verdict)
    string(JSON _ayther_exit GET "${_ayther_expected}" exit_code)

    # The hang vector never answers: the parent enforces the time limit and
    # the verdict is 'timeout'. Two seconds stand in for the contract's ten.
    set(_ayther_timeout "${_ayther_vector_limit}")
    if(_ayther_verdict STREQUAL "timeout")
        set(_ayther_timeout 2)
    endif()
    execute_process(COMMAND "${_ayther_probe_tool}" "${_ayther_vector_file}"
        RESULT_VARIABLE _ayther_result
        OUTPUT_VARIABLE _ayther_output
        ERROR_QUIET
        TIMEOUT ${_ayther_timeout})

    if(_ayther_verdict STREQUAL "timeout")
        if(NOT _ayther_result MATCHES "timeout")
            message(FATAL_ERROR
                "${_ayther_id}: the supervising parent must stop the probe, "
                "got '${_ayther_result}'")
        endif()
        math(EXPR _ayther_vectors_run "${_ayther_vectors_run} + 1")
        continue()
    endif()

    if(NOT _ayther_result EQUAL _ayther_exit)
        message(FATAL_ERROR
            "${_ayther_id}: expected exit ${_ayther_exit}, got ${_ayther_result}")
    endif()
    string(REGEX MATCH "AYTHER_STATUS (\{[^\r\n]*\})" _ayther_status "${_ayther_output}")
    if(_ayther_status STREQUAL "")
        message(FATAL_ERROR "${_ayther_id}: no AYTHER_STATUS line: ${_ayther_output}")
    endif()
    set(_ayther_status "${CMAKE_MATCH_1}")

    string(JSON _ayther_protocol GET "${_ayther_status}" protocol_version)
    if(NOT _ayther_protocol EQUAL 2)
        message(FATAL_ERROR "${_ayther_id}: protocol_version ${_ayther_protocol}")
    endif()
    string(JSON _ayther_actual_verdict GET "${_ayther_status}" verdict)
    if(NOT _ayther_actual_verdict STREQUAL _ayther_verdict)
        message(FATAL_ERROR
            "${_ayther_id}: expected verdict ${_ayther_verdict}, got "
            "${_ayther_actual_verdict}: ${_ayther_status}")
    endif()
    # Every other expected field must be present and equal.
    foreach(_ayther_field IN ITEMS reason ayther_abi capabilities)
        string(JSON _ayther_want ERROR_VARIABLE _ayther_no_want
            GET "${_ayther_expected}" ${_ayther_field})
        if(NOT _ayther_no_want STREQUAL "NOTFOUND")
            continue()
        endif()
        string(JSON _ayther_have ERROR_VARIABLE _ayther_no_have
            GET "${_ayther_status}" ${_ayther_field})
        if(NOT _ayther_no_have STREQUAL "NOTFOUND" OR
           NOT _ayther_have STREQUAL _ayther_want)
            message(FATAL_ERROR
                "${_ayther_id}: ${_ayther_field} expected '${_ayther_want}', "
                "got '${_ayther_have}': ${_ayther_status}")
        endif()
    endforeach()
    string(JSON _ayther_want_missing ERROR_VARIABLE _ayther_no_want_missing
        GET "${_ayther_expected}" missing_capabilities)
    if(_ayther_no_want_missing STREQUAL "NOTFOUND")
        string(JSON _ayther_have_missing GET "${_ayther_status}" missing_capabilities)
        string(JSON _ayther_want_text GET "${_ayther_expected}" missing_capabilities)
        string(REGEX REPLACE "[ \r\n\t]" "" _ayther_want_text "${_ayther_want_text}")
        string(REGEX REPLACE "[ \r\n\t]" "" _ayther_have_missing "${_ayther_have_missing}")
        if(NOT _ayther_have_missing STREQUAL _ayther_want_text)
            message(FATAL_ERROR
                "${_ayther_id}: missing_capabilities expected ${_ayther_want_text}, "
                "got ${_ayther_have_missing}")
        endif()
    endif()
    math(EXPR _ayther_vectors_run "${_ayther_vectors_run} + 1")
endforeach()
if(NOT _ayther_vectors_run EQUAL 6)
    message(FATAL_ERROR
        "Expected to run 6 installed probe vectors, ran ${_ayther_vectors_run}")
endif()
message(STATUS "Installed core probe: ${_ayther_vectors_run} vectors match")

