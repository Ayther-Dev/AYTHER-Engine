# Recording-based GPU oracle for focus brightness and opacity.
# Requires a Vulkan device and a caller-supplied recording.
add_executable(focus_layer_smoke main.cpp)
target_link_libraries(focus_layer_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(focus_layer_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(focus_layer_smoke PRIVATE cxx_std_20)
target_compile_definitions(focus_layer_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(focus_layer_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
