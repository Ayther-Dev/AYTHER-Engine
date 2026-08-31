cmake_minimum_required(VERSION 3.21)

if(NOT AYTHER_PACKAGE_PREFIX)
    message(FATAL_ERROR "Missing -DAYTHER_PACKAGE_PREFIX=<installed prefix>")
endif()

# 0.2 is outside the source-compatible 0.1.x window. Quiet mode lets this
# script assert the rejection and emit one stable, actionable diagnostic.
find_package(Ayther 0.2 EXACT QUIET CONFIG
    PATHS "${AYTHER_PACKAGE_PREFIX}"
    NO_DEFAULT_PATH)

if(Ayther_FOUND)
    message(FATAL_ERROR
        "Incompatible package accepted: requested 0.2 but found ${Ayther_VERSION}")
endif()

if(NOT "0.1.0" IN_LIST Ayther_CONSIDERED_VERSIONS)
    message(FATAL_ERROR
        "The installed Ayther package was not considered at ${AYTHER_PACKAGE_PREFIX}")
endif()

message("  [ OK ] incompatible package rejected: requested 0.2, installed 0.1.0; "
        "Runtime must use headers and library from the same 0.1.x package")
