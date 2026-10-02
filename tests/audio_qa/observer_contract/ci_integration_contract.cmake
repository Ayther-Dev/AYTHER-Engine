cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED QA_ENGINE_SOURCE_DIR)
    message(FATAL_ERROR "QA_ENGINE_SOURCE_DIR is required")
endif()

file(READ "${QA_ENGINE_SOURCE_DIR}/.github/workflows/ci.yml" workflow_text)

foreach(token IN ITEMS
        "-DQA_REQUIRED_FAMILIES=audio_qa,unit,contract,integration,limits,fidelity,performance"
        "cmake --build --preset"
        "ctest --preset"
        "Test native targets"
        "Install the native package"
        "Configure the out-of-tree package consumer"
        "Build the out-of-tree package consumer"
        "Run the out-of-tree package consumer"
        "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"
        "required-ci:"
        "- native")
    string(FIND "${workflow_text}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "Engine CI integration contract omits '${token}'")
    endif()
endforeach()

string(REGEX MATCHALL
    "name: (Windows|Linux) native( \\+ VPX)? \\+ package consumer"
    consumer_matrix_entries "${workflow_text}")
list(LENGTH consumer_matrix_entries consumer_matrix_count)
if(NOT consumer_matrix_count EQUAL 4)
    message(FATAL_ERROR
        "Engine CI must build and run four native package consumers; found ${consumer_matrix_count}")
endif()

string(FIND "${workflow_text}" "  native:" native_begin)
string(FIND "${workflow_text}" "  required-ci:" required_begin)
if(native_begin EQUAL -1 OR required_begin EQUAL -1 OR
   required_begin LESS_EQUAL native_begin)
    message(FATAL_ERROR "Engine CI native and required gates are malformed")
endif()
math(EXPR native_length "${required_begin} - ${native_begin}")
string(SUBSTRING "${workflow_text}" ${native_begin} ${native_length}
    native_job)
string(FIND "${native_job}" "continue-on-error" non_blocking_position)
if(NOT non_blocking_position EQUAL -1)
    message(FATAL_ERROR
        "Engine native build, tests and consumers must remain blocking")
endif()

message(STATUS
    "Engine CI builds native QA, runs critical CTest families and validates installed consumers")
