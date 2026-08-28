# Installation and package configuration for the core-only and native-engine
# surfaces. Consumers need neither Cargo nor Corrosion after installation.
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

install(FILES
        "${PROJECT_SOURCE_DIR}/include/ayther/ayther_core_ffi.h"
        "${PROJECT_SOURCE_DIR}/include/ayther/ayther_version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ayther")
install(FILES "${AYTHER_CORE_ARCHIVE}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")

set(AYTHER_PACKAGE_HAS_ENGINE OFF)
set(AYTHER_PACKAGE_HAS_VPX OFF)
set(AYTHER_PACKAGE_VPX_BUNDLED OFF)
set(AYTHER_VPX_ARCHIVE_NAME "")

if(AYTHER_BUILD_ENGINE)
    set(AYTHER_PACKAGE_HAS_ENGINE ON)

    install(TARGETS ayther_engine ayther_ymfm
        EXPORT AytherEngineTargets
        ARCHIVE       DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ayther"
        INCLUDES      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

    install(EXPORT AytherEngineTargets
        FILE AytherEngineTargets.cmake
        NAMESPACE Ayther::
        DESTINATION "${AYTHER_INSTALL_CMAKEDIR}")

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/ymfm/src/"
        DESTINATION
            "${CMAKE_INSTALL_INCLUDEDIR}/ayther/third_party/ymfm"
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.ipp")

    install(FILES ${AYTHER_SHADER_BINARIES}
        DESTINATION "${CMAKE_INSTALL_DATADIR}/Ayther/shaders")

    install(FILES
        "${PROJECT_SOURCE_DIR}/third_party/ymfm/LICENSE"
        "${PROJECT_SOURCE_DIR}/third_party/ymfm/README.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/Ayther/ymfm")

    if(AYTHER_ENABLE_VPX)
        set(AYTHER_PACKAGE_HAS_VPX ON)
        set(AYTHER_PACKAGE_VPX_BUNDLED "${AYTHER_VPX_BUNDLED}")

        if(AYTHER_VPX_BUNDLED)
            get_filename_component(AYTHER_VPX_ARCHIVE_NAME
                "${_ayther_vpx_library}" NAME)
            install(FILES "${_ayther_vpx_library}"
                DESTINATION "${CMAKE_INSTALL_LIBDIR}")
            install(DIRECTORY "${AYTHER_VPX_ROOT}/include/vpx"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

            foreach(_ayther_vpx_notice IN ITEMS VERSION LICENSE PATENTS AUTHORS)
                if(EXISTS "${AYTHER_VPX_ROOT}/${_ayther_vpx_notice}")
                    install(FILES
                        "${AYTHER_VPX_ROOT}/${_ayther_vpx_notice}"
                        DESTINATION
                            "${CMAKE_INSTALL_DATADIR}/licenses/Ayther/libvpx")
                endif()
            endforeach()
        endif()
    endif()

    # Preserve the complete vcpkg license set selected by this exact manifest
    # and triplet. The files are renamed by port so they coexist in one folder.
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        file(GLOB _ayther_vcpkg_copyrights
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
        foreach(_ayther_vcpkg_copyright IN LISTS _ayther_vcpkg_copyrights)
            get_filename_component(_ayther_vcpkg_share_dir
                "${_ayther_vcpkg_copyright}" DIRECTORY)
            get_filename_component(_ayther_vcpkg_port
                "${_ayther_vcpkg_share_dir}" NAME)
            install(FILES "${_ayther_vcpkg_copyright}"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/Ayther/vcpkg"
                RENAME "${_ayther_vcpkg_port}.txt")
        endforeach()
    endif()
endif()

install(FILES
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/NOTICE.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/Ayther")

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
unset(_ayther_vcpkg_copyright)
unset(_ayther_vcpkg_copyrights)
unset(_ayther_vcpkg_port)
unset(_ayther_vcpkg_share_dir)
unset(_ayther_vpx_notice)
