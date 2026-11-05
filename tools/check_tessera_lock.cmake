# Tessera lock test (TP-016.1).
#
#   cmake -DAYTHER_TESSERA_LOCK=<dependencies/tessera.lock.json>
#         [-DAYTHER_TESSERA_PREFIX=<installed Tessera package>]
#         -P tools/check_tessera_lock.cmake
#
# Fails, naming each problem, when the lock is malformed or when the installed
# package is not the one the lock names (commit, package version, API version).
cmake_minimum_required(VERSION 3.21)

if(NOT AYTHER_TESSERA_LOCK)
    message(FATAL_ERROR "Missing -DAYTHER_TESSERA_LOCK=<tessera.lock.json>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
include(AytherTesseraLock)

if(AYTHER_TESSERA_PREFIX)
    ayther_require_tessera_api(
        ${AYTHER_TESSERA_MIN_API_MAJOR} ${AYTHER_TESSERA_MIN_API_MINOR}
        "${AYTHER_TESSERA_PREFIX}")
endif()

ayther_verify_tessera_lock(_problems
    LOCK "${AYTHER_TESSERA_LOCK}"
    PREFIX "${AYTHER_TESSERA_PREFIX}")

if(_problems)
    list(JOIN _problems "\n  " _text)
    message(FATAL_ERROR "The Tessera lock check failed:\n  ${_text}")
endif()

if(AYTHER_TESSERA_PREFIX)
    message(STATUS "Tessera lock: ${AYTHER_TESSERA_LOCK} matches ${AYTHER_TESSERA_PREFIX}")
else()
    message(STATUS "Tessera lock: ${AYTHER_TESSERA_LOCK} is well formed")
endif()
