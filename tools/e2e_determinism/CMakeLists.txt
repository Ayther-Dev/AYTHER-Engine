# ---------------------------------------------------------------------------
# e2e_determinism — the whole pipeline, hashed, twice.
#
# Runs a synthetic ROM through the test core with a signed pack and a scripted
# input track, then compares frames, audio, events, and RAM against both the
# other run in the same process and a set of pinned hashes.
# ---------------------------------------------------------------------------
add_executable(e2e_determinism main.cpp)

target_link_libraries(e2e_determinism PRIVATE
    ayther_engine
    ayther_core
    ayther_test_support)

if(WIN32)
    target_link_libraries(e2e_determinism PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_include_directories(e2e_determinism PRIVATE
    "${CMAKE_SOURCE_DIR}/tools/common")
target_compile_features(e2e_determinism PRIVATE cxx_std_20)
target_compile_definitions(e2e_determinism PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set_target_properties(e2e_determinism PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The pinned answer, recorded on Windows with clang-cl on 2026-08-30 and
# asserted from then on. CI running the same binary on Linux is what turns these
# into a CROSS-PLATFORM claim rather than a local one.
#
# A mismatch here is a finding, not a number to refresh. It means one of three
# things, and all three are worth knowing: the engine changed behaviour, the
# test core changed, or the two platforms disagree. Re-pinning without deciding
# which converts this oracle into a rubber stamp.
#
# They are overridable so a deliberate behaviour change can be re-pinned in one
# place, and so the run-to-run half can still be exercised with them off.
set(AYTHER_E2E_GOLDEN_FRAMES "0x54293E3A8D610D83ULL" CACHE STRING
    "Pinned frame hash for the end-to-end determinism oracle")
set(AYTHER_E2E_GOLDEN_AUDIO  "0x836BB4864D06401FULL" CACHE STRING
    "Pinned audio hash for the end-to-end determinism oracle")
set(AYTHER_E2E_GOLDEN_EVENTS "0x3E6FD3C84F1FBF49ULL" CACHE STRING
    "Pinned event hash for the end-to-end determinism oracle")
set(AYTHER_E2E_GOLDEN_RAM    "0x5D4A3019E5F13B43ULL" CACHE STRING
    "Pinned work-RAM hash for the end-to-end determinism oracle")

if(AYTHER_E2E_GOLDEN_FRAMES)
    target_compile_definitions(e2e_determinism PRIVATE
        AYTHER_E2E_GOLDEN_FRAMES=${AYTHER_E2E_GOLDEN_FRAMES}
        AYTHER_E2E_GOLDEN_AUDIO=${AYTHER_E2E_GOLDEN_AUDIO}
        AYTHER_E2E_GOLDEN_EVENTS=${AYTHER_E2E_GOLDEN_EVENTS}
        AYTHER_E2E_GOLDEN_RAM=${AYTHER_E2E_GOLDEN_RAM})
endif()

ayther_abi_test_env(_e2e_env)
add_test(NAME e2e_determinismo COMMAND e2e_determinism)
set_tests_properties(e2e_determinismo PROPERTIES
    ENVIRONMENT      "${_e2e_env}"
    SKIP_RETURN_CODE 77
    TIMEOUT          300
    LABELS           "engine;native;integration;determinism")
