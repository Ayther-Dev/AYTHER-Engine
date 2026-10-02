cmake_minimum_required(VERSION 3.25)

foreach(required_variable IN ITEMS
        QA_BUILD_DIRECTORY
        QA_CTEST_COMMAND
        QA_REQUIRED_FAMILIES)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required.")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${QA_BUILD_DIRECTORY}")
    message(FATAL_ERROR
        "QA_BUILD_DIRECTORY does not exist: '${QA_BUILD_DIRECTORY}'.")
endif()

string(REPLACE "," ";" required_families "${QA_REQUIRED_FAMILIES}")
foreach(family IN LISTS required_families)
    if(NOT family MATCHES "^[A-Za-z0-9_+.-]+$")
        message(FATAL_ERROR "Invalid CTest family name: '${family}'.")
    endif()

    set(discovery_command
        "${QA_CTEST_COMMAND}"
        --test-dir "${QA_BUILD_DIRECTORY}")
    if(DEFINED QA_CONFIGURATION AND NOT "${QA_CONFIGURATION}" STREQUAL "")
        list(APPEND discovery_command -C "${QA_CONFIGURATION}")
    endif()
    list(APPEND discovery_command
        --show-only=json-v1
        -L "^${family}$")

    execute_process(
        COMMAND ${discovery_command}
        RESULT_VARIABLE discovery_result
        OUTPUT_VARIABLE discovery_output
        ERROR_VARIABLE discovery_error)
    if(NOT discovery_result EQUAL 0)
        message(FATAL_ERROR
            "CTest could not discover family '${family}' (exit "
            "${discovery_result}).\n${discovery_error}")
    endif()

    string(JSON test_count ERROR_VARIABLE json_error
        LENGTH "${discovery_output}" tests)
    if(json_error)
        message(FATAL_ERROR
            "CTest returned invalid JSON for family '${family}': "
            "${json_error}")
    endif()
    if(test_count EQUAL 0)
        message(FATAL_ERROR
            "Required CTest family '${family}' discovered zero tests.")
    endif()

    message(STATUS
        "Required CTest family '${family}': ${test_count} test(s) discovered.")
endforeach()
