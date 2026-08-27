# ---------------------------------------------------------------------------
# AytherCargo.cmake - build the Rust ayther_core crate and expose it to CMake.
#
# `ayther_add_rust_core()` runs `cargo build` for the `ayther_core` package and
# publishes the resulting static archive as the IMPORTED target `Ayther::core`,
# carrying the public header directory and the system libraries the Rust
# runtime needs.
#
# The crate is built into the workspace-standard `target/` directory rather than
# per-preset, so presets that select the same cargo profile share one compiled
# dependency graph instead of rebuilding vendored Lua for each of them.
# `target/` is already ignored by .gitignore.
# ---------------------------------------------------------------------------

find_program(AYTHER_CARGO_EXECUTABLE
    NAMES cargo
    HINTS "$ENV{CARGO_HOME}/bin"
          "$ENV{USERPROFILE}/.cargo/bin"
          "$ENV{HOME}/.cargo/bin"
    DOC   "Path to the cargo executable")

if(NOT AYTHER_CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "cargo was not found. Install the Rust toolchain selected by "
        "rust-toolchain.toml; see docs/DEVELOPMENT_ENVIRONMENT.md.")
endif()

# The Rust runtime pulls in these platform libraries. Regenerate the list with:
#
#     cargo rustc -p ayther_core --lib --crate-type staticlib \
#         -- --print native-static-libs
#
# and override at configure time with -DAYTHER_CORE_SYSTEM_LIBS="a;b;c".
if(WIN32)
    set(_ayther_default_system_libs
        kernel32 ntdll userenv ws2_32 dbghelp bcrypt advapi32 secur32)
elseif(APPLE)
    set(_ayther_default_system_libs "")
else()
    set(_ayther_default_system_libs pthread dl m)
endif()

set(AYTHER_CORE_SYSTEM_LIBS "${_ayther_default_system_libs}" CACHE STRING
    "Platform libraries required when linking the ayther_core static archive")

function(ayther_add_rust_core)
    get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_multi_config)
        message(FATAL_ERROR
            "The cargo integration supports single-configuration generators "
            "only, because the cargo profile is chosen at configure time. All "
            "shared presets use Ninja; select one of them.")
    endif()

    # Map the CMake build type onto a cargo profile and its output directory.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_cargo_profile_args "")
        set(_cargo_profile_dir  "debug")
    else()
        set(_cargo_profile_args "--release")
        set(_cargo_profile_dir  "release")
    endif()

    set(_cargo_target_dir "${PROJECT_SOURCE_DIR}/target")
    set(_lib_path
        "${_cargo_target_dir}/${_cargo_profile_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}ayther_core${CMAKE_STATIC_LIBRARY_SUFFIX}")

    if(AYTHER_CARGO_LOCKED)
        list(APPEND _cargo_profile_args "--locked")
    endif()

    # Rebuild whenever any crate input changes. CONFIGURE_DEPENDS re-runs the
    # glob when the build system is regenerated, so new .rs files are picked up.
    file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/core/src/*.rs")
    list(APPEND _rust_sources
        "${PROJECT_SOURCE_DIR}/core/Cargo.toml"
        "${PROJECT_SOURCE_DIR}/core/build.rs"
        "${PROJECT_SOURCE_DIR}/Cargo.toml"
        "${PROJECT_SOURCE_DIR}/Cargo.lock"
        "${PROJECT_SOURCE_DIR}/rust-toolchain.toml")

    add_custom_command(
        OUTPUT  "${_lib_path}"
        COMMAND "${CMAKE_COMMAND}" -E env
                    "CARGO_TARGET_DIR=${_cargo_target_dir}"
                    "${AYTHER_CARGO_EXECUTABLE}" build
                    --package ayther_core --lib ${_cargo_profile_args}
        DEPENDS ${_rust_sources}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "cargo build ayther_core (${_cargo_profile_dir})"
        USES_TERMINAL
        VERBATIM)

    add_custom_target(ayther_core_cargo DEPENDS "${_lib_path}")

    # An IMPORTED target cannot be built by CMake, so the dependency on the
    # cargo step is followed transitively by everything that links it.
    add_library(ayther_core STATIC IMPORTED GLOBAL)
    set_target_properties(ayther_core PROPERTIES
        IMPORTED_LOCATION                 "${_lib_path}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
        INTERFACE_INCLUDE_DIRECTORIES     "${PROJECT_SOURCE_DIR}/include"
        INTERFACE_LINK_LIBRARIES          "${AYTHER_CORE_SYSTEM_LIBS}")
    add_dependencies(ayther_core ayther_core_cargo)
    add_library(Ayther::core ALIAS ayther_core)

    set(AYTHER_CORE_STATIC_LIB  "${_lib_path}"         PARENT_SCOPE)
    set(AYTHER_CARGO_PROFILE_DIR "${_cargo_profile_dir}" PARENT_SCOPE)
endfunction()
