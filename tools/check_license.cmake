# check_license.cmake — verify the Engine repository's license declarations.
#
# Repository separation is the legal boundary. This check intentionally does
# not assume that AYTHER Lab, SDK, Runtime, Play, or Hub exist as subdirectories.
#
# Usage:
#   cmake -DAYTHER_REPO=<dir> -P tools/check_license.cmake

if(NOT AYTHER_REPO)
    set(AYTHER_REPO "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

set(_license "${AYTHER_REPO}/LICENSE")
if(NOT EXISTS "${_license}")
    message(FATAL_ERROR "LICENSE is missing from the Engine repository root")
endif()

file(READ "${_license}" _license_text)
string(LENGTH "${_license_text}" _license_length)
if(_license_length LESS 200)
    message(FATAL_ERROR
        "LICENSE has only ${_license_length} characters and appears truncated")
endif()

file(READ "${AYTHER_REPO}/Cargo.toml" _cargo)
if(NOT _cargo MATCHES "\n[ \t]*license[ \t]*=[ \t]*\"([^\"]+)\"")
    message(FATAL_ERROR
        "Cargo.toml does not declare a workspace license while LICENSE exists")
endif()
set(_spdx "${CMAKE_MATCH_1}")

string(TOUPPER "${_license_text}" _license_upper)
string(TOUPPER "${_spdx}" _spdx_upper)
string(REGEX REPLACE "-[0-9.]+$" "" _license_family "${_spdx_upper}")
if(NOT _license_upper MATCHES "${_license_family}")
    message(FATAL_ERROR
        "Cargo declares ${_spdx}, but LICENSE does not mention ${_license_family}")
endif()

set(_legal "${AYTHER_REPO}/docs/LEGAL_AND_DISTRIBUTION.md")
if(NOT EXISTS "${_legal}")
    message(FATAL_ERROR "docs/LEGAL_AND_DISTRIBUTION.md is missing")
endif()
file(READ "${_legal}" _legal_text)
if(NOT _legal_text MATCHES "${_spdx}")
    message(FATAL_ERROR
        "LEGAL_AND_DISTRIBUTION.md does not name Cargo's ${_spdx} license")
endif()

message("  [ OK ] ${_spdx} is consistent across LICENSE, Cargo.toml, and Engine policy")
