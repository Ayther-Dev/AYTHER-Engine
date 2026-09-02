cmake_minimum_required(VERSION 3.21)

project(AytherPackageConsumer LANGUAGES CXX)

# Package discovery must be deterministic and independent of a developer's
# user package registry. The installed prefix is supplied by the caller.
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)

# A name outside the published component set must be rejected, including the
# historical product-layer name that is deliberately not part of this package.
find_package(Ayther 0.1.0 QUIET CONFIG COMPONENTS frontend)
if(Ayther_FOUND)
    message(FATAL_ERROR "Ayther accepted the unknown 'frontend' component")
endif()
if(NOT Ayther_NOT_FOUND_MESSAGE MATCHES "Unsupported Ayther component")
    message(FATAL_ERROR
        "Unknown component rejection did not provide the expected diagnostic: "
        "${Ayther_NOT_FOUND_MESSAGE}")
endif()

find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)

if(NOT Ayther_engine_FOUND STREQUAL "TRUE")
    message(FATAL_ERROR
        "Ayther_engine_FOUND must be TRUE, got '${Ayther_engine_FOUND}'")
endif()
if(NOT TARGET Ayther::engine)
    message(FATAL_ERROR "The engine component did not import Ayther::engine")
endif()
if(TARGET Ayther::frontend)
    message(FATAL_ERROR "The installed package must not define a frontend target")
endif()

# The static Engine's complete installed link closure uses stable, official
# imported target names. In particular, SDL's convenience alias is normalized
# through a package adapter instead of resolving to a shared/static detail.
get_target_property(_ayther_engine_links
    Ayther::engine INTERFACE_LINK_LIBRARIES)
set(_ayther_normalized_engine_links "")
foreach(_ayther_link IN LISTS _ayther_engine_links)
    if(_ayther_link MATCHES "^\\$<LINK_ONLY:(.*)>$")
        set(_ayther_link "${CMAKE_MATCH_1}")
    endif()
    if(NOT _ayther_link STREQUAL "")
        list(APPEND _ayther_normalized_engine_links "${_ayther_link}")
    endif()
endforeach()

set(_ayther_required_engine_links
    Threads::Threads
    Ayther::sdl3
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    tomlplusplus::tomlplusplus
    zstd::libzstd)
foreach(_ayther_link IN LISTS _ayther_required_engine_links)
    if(NOT _ayther_link IN_LIST _ayther_normalized_engine_links)
        message(FATAL_ERROR
            "Ayther::engine is missing transitive target ${_ayther_link}: "
            "${_ayther_normalized_engine_links}")
    endif()
endforeach()

get_target_property(_ayther_sdl_link
    Ayther::sdl3 INTERFACE_LINK_LIBRARIES)
if(NOT _ayther_sdl_link STREQUAL "SDL3::SDL3")
    message(FATAL_ERROR
        "Ayther::sdl3 must normalize SDL to SDL3::SDL3, got "
        "'${_ayther_sdl_link}'")
endif()

foreach(_ayther_link IN LISTS _ayther_normalized_engine_links)
    if(_ayther_link MATCHES "^SDL3::SDL3-(shared|static)$")
        message(FATAL_ERROR
            "Ayther::engine leaks an SDL implementation target: ${_ayther_link}")
    endif()
endforeach()

# Imported path-bearing usage requirements may point inside the selected
# install prefix, but nowhere else. In particular, a producer checkout or
# build-directory path must never survive installation.
get_filename_component(_ayther_package_prefix "${AYTHER_INCLUDE_DIR}" DIRECTORY)
cmake_path(NORMAL_PATH _ayther_package_prefix)

# Ayther::engine publishes one include root and nothing more specific. Public
# headers must not require source, backend, or monorepo include directories.
set(_ayther_expected_include_dir "${_ayther_package_prefix}/include")
cmake_path(NORMAL_PATH _ayther_expected_include_dir)
get_target_property(_ayther_engine_include_dirs
    Ayther::engine INTERFACE_INCLUDE_DIRECTORIES)
set(_ayther_normalized_include_dirs "")
foreach(_ayther_include_dir IN LISTS _ayther_engine_include_dirs)
    cmake_path(NORMAL_PATH _ayther_include_dir)
    list(APPEND _ayther_normalized_include_dirs "${_ayther_include_dir}")
endforeach()
list(REMOVE_DUPLICATES _ayther_normalized_include_dirs)
if(NOT _ayther_normalized_include_dirs STREQUAL _ayther_expected_include_dir)
    message(FATAL_ERROR
        "Ayther::engine must publish only '${_ayther_expected_include_dir}', "
        "got '${_ayther_normalized_include_dirs}'")
endif()

set(_ayther_required_public_headers
    ayther/ayther_session.h
    ayther/ayther_layers.h
    ayther/ayther_core_ffi.h
    ayther/ayther_renderer.h
    ayther/engine/engine.hpp
    ayther/engine/capabilities.hpp
    ayther/engine/core_probe.hpp
    ayther/engine/input.hpp
    ayther/engine/pack.hpp
    ayther/engine/vulkan_interop.hpp)
foreach(_ayther_header IN LISTS _ayther_required_public_headers)
    if(NOT EXISTS "${_ayther_expected_include_dir}/${_ayther_header}")
        message(FATAL_ERROR
            "Installed public header is missing: ${_ayther_header}")
    endif()
endforeach()
set(_ayther_path_properties
    INTERFACE_INCLUDE_DIRECTORIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    INTERFACE_LINK_DIRECTORIES
    INTERFACE_LINK_DEPENDS
    INTERFACE_LINK_LIBRARIES
    INTERFACE_PRECOMPILE_HEADERS
    INTERFACE_SOURCES)
foreach(_ayther_property IN LISTS _ayther_path_properties)
    get_target_property(_ayther_values Ayther::engine "${_ayther_property}")
    if(NOT _ayther_values OR _ayther_values MATCHES "-NOTFOUND$")
        continue()
    endif()

    foreach(_ayther_value IN LISTS _ayther_values)
        if(_ayther_value MATCHES "^\\$<LINK_ONLY:(.*)>$")
            set(_ayther_path "${CMAKE_MATCH_1}")
        else()
            string(GENEX_STRIP "${_ayther_value}" _ayther_path)
        endif()
        if(_ayther_path STREQUAL "" OR NOT IS_ABSOLUTE "${_ayther_path}")
            if(_ayther_property STREQUAL "INTERFACE_LINK_LIBRARIES" AND
               NOT _ayther_path STREQUAL "" AND
               NOT TARGET "${_ayther_path}")
                message(FATAL_ERROR
                    "Ayther::engine links a non-target interface item: "
                    "${_ayther_path}")
            endif()
            continue()
        endif()

        cmake_path(IS_PREFIX _ayther_package_prefix "${_ayther_path}"
            NORMALIZE _ayther_is_installed_path)
        if(NOT _ayther_is_installed_path)
            message(FATAL_ERROR
                "Ayther::engine ${_ayther_property} leaks a path outside the "
                "install prefix: ${_ayther_path}")
        endif()
    endforeach()
endforeach()

# Relocatability is also a property of the generated package files themselves:
# none may embed the producer checkout/build root or publish a frontend target.
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.." _ayther_producer_root)
file(TO_CMAKE_PATH "${_ayther_producer_root}" _ayther_producer_root)
file(GLOB _ayther_installed_cmake_files "${Ayther_DIR}/*.cmake")
set(_ayther_required_find_dependencies
    "find_dependency(Threads)"
    "find_dependency(SDL3 CONFIG)"
    "find_dependency(Vulkan)"
    "find_dependency(VulkanMemoryAllocator CONFIG)"
    "find_dependency(tomlplusplus CONFIG)"
    "find_dependency(zstd CONFIG)")
file(READ "${Ayther_DIR}/AytherConfig.cmake" _ayther_package_config_contents)
foreach(_ayther_find_dependency IN LISTS _ayther_required_find_dependencies)
    string(FIND "${_ayther_package_config_contents}"
        "${_ayther_find_dependency}" _ayther_find_dependency_offset)
    if(_ayther_find_dependency_offset EQUAL -1)
        message(FATAL_ERROR
            "AytherConfig.cmake is missing ${_ayther_find_dependency}")
    endif()
endforeach()

foreach(_ayther_cmake_file IN LISTS _ayther_installed_cmake_files)
    file(READ "${_ayther_cmake_file}" _ayther_cmake_contents)
    string(REPLACE "\\" "/" _ayther_cmake_contents
        "${_ayther_cmake_contents}")
    string(FIND "${_ayther_cmake_contents}" "${_ayther_producer_root}"
        _ayther_producer_path_offset)
    if(NOT _ayther_producer_path_offset EQUAL -1)
        message(FATAL_ERROR
            "Installed CMake file embeds the producer tree: "
            "${_ayther_cmake_file}")
    endif()
    if(_ayther_cmake_contents MATCHES "Ayther::frontend")
        message(FATAL_ERROR
            "Installed CMake file publishes a frontend target: "
            "${_ayther_cmake_file}")
    endif()
    string(TOLOWER "${_ayther_cmake_contents}" _ayther_cmake_contents_lower)
    foreach(_ayther_forbidden_surface IN ITEMS
            "vk-bootstrap" "stb::" "imgui" "vcpkg_")
        string(FIND "${_ayther_cmake_contents_lower}"
            "${_ayther_forbidden_surface}" _ayther_forbidden_surface_offset)
        if(NOT _ayther_forbidden_surface_offset EQUAL -1)
            message(FATAL_ERROR
                "Installed CMake file exposes forbidden dependency surface "
                "'${_ayther_forbidden_surface}': ${_ayther_cmake_file}")
        endif()
    endforeach()
endforeach()

if(NOT DEFINED Ayther_SHADER_DIR OR Ayther_SHADER_DIR STREQUAL "")
    message(FATAL_ERROR
        "The installed engine package did not export Ayther_SHADER_DIR")
endif()
if(NOT IS_ABSOLUTE "${Ayther_SHADER_DIR}")
    message(FATAL_ERROR
        "Ayther_SHADER_DIR is not absolute: ${Ayther_SHADER_DIR}")
endif()
set(_ayther_expected_shader_dir
    "${_ayther_package_prefix}/share/Ayther/shaders")
cmake_path(NORMAL_PATH _ayther_expected_shader_dir)
cmake_path(NORMAL_PATH Ayther_SHADER_DIR)
if(NOT Ayther_SHADER_DIR STREQUAL _ayther_expected_shader_dir)
    message(FATAL_ERROR
        "Ayther_SHADER_DIR does not use the installed shader contract: "
        "expected '${_ayther_expected_shader_dir}', got '${Ayther_SHADER_DIR}'")
endif()

set(_ayther_required_shaders
    indexed_plane.frag.spv
    indexed_plane.vert.spv
    sprite.frag.spv
    sprite.vert.spv
    sprite_mask.frag.spv
    sprite_mult.frag.spv
    sprite_screen.frag.spv
    video.frag.spv)
foreach(_ayther_shader IN LISTS _ayther_required_shaders)
    if(NOT EXISTS "${Ayther_SHADER_DIR}/${_ayther_shader}")
        message(FATAL_ERROR
            "Installed Engine shader is missing: "
            "${Ayther_SHADER_DIR}/${_ayther_shader}")
    endif()
endforeach()

add_executable(ayther_package_consumer main.cpp)
target_compile_features(ayther_package_consumer PRIVATE cxx_std_20)
target_link_libraries(ayther_package_consumer PRIVATE Ayther::engine)

# Recompile every required installed header as the first and only include in a
# translation unit. These probes inherit include paths solely from the imported
# target, so a source-tree fallback cannot hide a missing package dependency.
foreach(_ayther_header_probe IN ITEMS
        capabilities_header
        core_ffi_header
        core_probe_header
        engine_header
        input_header
        layers_header
        pack_header
        renderer_header
        session_header
        vulkan_interop_header)
    add_library("ayther_installed_${_ayther_header_probe}" OBJECT
        "${CMAKE_CURRENT_LIST_DIR}/../contracts/${_ayther_header_probe}.cpp")
    target_compile_features("ayther_installed_${_ayther_header_probe}"
        PRIVATE cxx_std_20)
    target_link_libraries("ayther_installed_${_ayther_header_probe}"
        PRIVATE Ayther::engine)
endforeach()

# This is the Runtime-side deployment contract: the package variable is the
# sole source path. Consumers must not reconstruct share/Ayther/shaders from a
# checkout, build directory, executable path, or vcpkg layout.
add_custom_command(TARGET ayther_package_consumer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:ayther_package_consumer>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${Ayther_SHADER_DIR}"
        "$<TARGET_FILE_DIR:ayther_package_consumer>/shaders"
    VERBATIM)
