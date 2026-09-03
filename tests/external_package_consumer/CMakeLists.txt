cmake_minimum_required(VERSION 3.21)

project(AytherExternalPackageConsumer LANGUAGES CXX)

# This project is copied outside the Engine checkout by the release gate. Its
# configure/build therefore has no source-tree fallback available.
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)

if(NOT Ayther_engine_FOUND STREQUAL "TRUE" OR NOT TARGET Ayther::engine)
    message(FATAL_ERROR "The installed package did not import Ayther::engine")
endif()

add_executable(ayther_external_package_consumer main.cpp)
target_compile_features(ayther_external_package_consumer PRIVATE cxx_std_20)
target_link_libraries(ayther_external_package_consumer PRIVATE Ayther::engine)

# Record every imported target visible after find_package(), including its
# path-bearing and link-interface properties. A later, independent PowerShell
# step scans this report together with both consumer CMakeCache.txt files.
get_property(_ayther_imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
list(APPEND _ayther_imported_targets Ayther::engine)
list(REMOVE_DUPLICATES _ayther_imported_targets)
list(SORT _ayther_imported_targets)

set(_ayther_audit_properties
    IMPORTED_IMPLIB
    IMPORTED_LOCATION
    IMPORTED_OBJECTS
    INTERFACE_COMPILE_DEFINITIONS
    INTERFACE_COMPILE_OPTIONS
    INTERFACE_INCLUDE_DIRECTORIES
    INTERFACE_LINK_DEPENDS
    INTERFACE_LINK_DIRECTORIES
    INTERFACE_LINK_LIBRARIES
    INTERFACE_LINK_OPTIONS
    INTERFACE_PRECOMPILE_HEADERS
    INTERFACE_SOURCES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)

set(_ayther_target_report "")
foreach(_ayther_target IN LISTS _ayther_imported_targets)
    if(NOT TARGET "${_ayther_target}")
        continue()
    endif()

    set(_ayther_target_properties ${_ayther_audit_properties})
    get_target_property(_ayther_imported_configs
        "${_ayther_target}" IMPORTED_CONFIGURATIONS)
    foreach(_ayther_config IN LISTS _ayther_imported_configs)
        string(TOUPPER "${_ayther_config}" _ayther_config_upper)
        list(APPEND _ayther_target_properties
            "IMPORTED_IMPLIB_${_ayther_config_upper}"
            "IMPORTED_LOCATION_${_ayther_config_upper}"
            "IMPORTED_OBJECTS_${_ayther_config_upper}")
    endforeach()
    list(REMOVE_DUPLICATES _ayther_target_properties)

    foreach(_ayther_property IN LISTS _ayther_target_properties)
        get_target_property(_ayther_value
            "${_ayther_target}" "${_ayther_property}")
        if(_ayther_value AND NOT _ayther_value MATCHES "-NOTFOUND$")
            string(APPEND _ayther_target_report
                "${_ayther_target}|${_ayther_property}|${_ayther_value}\n")
        endif()
    endforeach()
endforeach()

file(WRITE "${CMAKE_BINARY_DIR}/imported-targets.txt"
    "${_ayther_target_report}")

