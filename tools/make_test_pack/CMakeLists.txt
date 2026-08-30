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

set_target_properties(make_test_pack PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
