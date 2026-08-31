# ---------------------------------------------------------------------------
# make_test_pack — produces signed content for the release-candidate consumer.
#
# Links only Ayther::core, so it builds under the headless presets too.
# ---------------------------------------------------------------------------
add_executable(make_test_pack main.cpp)

target_link_libraries(make_test_pack PRIVATE ayther_core)
if(WIN32)
    target_link_libraries(make_test_pack PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_include_directories(make_test_pack PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/ayther"
    "${PROJECT_SOURCE_DIR}/tools/common")
target_compile_features(make_test_pack PRIVATE cxx_std_20)

# The warnings policy, without the coverage/sanitiser instrumentation that
# ayther_instrument_target also brings. Taking only the policy keeps this target
# under warnings-as-errors while leaving the measured coverage denominator in
# docs/COVERAGE.md comparable; folding it into coverage is a separate decision.
target_link_libraries(make_test_pack PRIVATE
    $<BUILD_INTERFACE:ayther_strict_warnings>)

set_target_properties(make_test_pack PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
