# Installation and package configuration for the currently supported core
# surface. The C++ engine target will join this export only after it is complete.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(AYTHER_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Ayther")
set(AYTHER_CORE_ARCHIVE_NAME
    "${CMAKE_STATIC_LIBRARY_PREFIX}ayther_core${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(AYTHER_CORE_ARCHIVE
    "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/${AYTHER_CORE_ARCHIVE_NAME}")

set(AYTHER_CORE_SYSTEM_LIBS "")
set(AYTHER_CORE_LINK_OPTIONS "")
if(TARGET ayther_core-static)
    get_target_property(_ayther_core_links
        ayther_core-static INTERFACE_LINK_LIBRARIES)
    if(_ayther_core_links AND NOT _ayther_core_links MATCHES "-NOTFOUND$")
        foreach(_ayther_link IN LISTS _ayther_core_links)
            if(NOT TARGET "${_ayther_link}" AND
               NOT _ayther_link MATCHES "^\\$<")
                list(APPEND AYTHER_CORE_SYSTEM_LIBS "${_ayther_link}")
            endif()
        endforeach()
    endif()

    get_target_property(_ayther_core_options
        ayther_core-static INTERFACE_LINK_OPTIONS)
    if(_ayther_core_options AND NOT _ayther_core_options MATCHES "-NOTFOUND$")
        set(AYTHER_CORE_LINK_OPTIONS "${_ayther_core_options}")
    endif()
endif()
list(REMOVE_DUPLICATES AYTHER_CORE_SYSTEM_LIBS)

install(FILES "${PROJECT_SOURCE_DIR}/include/ayther/ayther_core_ffi.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ayther")
install(FILES "${AYTHER_CORE_ARCHIVE}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/AytherConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/AytherConfig.cmake"
    INSTALL_DESTINATION "${AYTHER_INSTALL_CMAKEDIR}"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/AytherConfigVersion.cmake"
    VERSION       "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion)

install(FILES
    "${PROJECT_BINARY_DIR}/AytherConfig.cmake"
    "${PROJECT_BINARY_DIR}/AytherConfigVersion.cmake"
    DESTINATION "${AYTHER_INSTALL_CMAKEDIR}")

unset(_ayther_core_links)
unset(_ayther_core_options)
unset(_ayther_link)
