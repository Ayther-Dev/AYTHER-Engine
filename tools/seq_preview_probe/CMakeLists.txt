# seq_preview_probe — energía del PCM de un span por los caminos del preview de
# Secuencia (por evento / por canal / mezcla). Diagnóstico de «no se escucha».
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(seq_preview_probe main.cpp)
target_link_libraries(seq_preview_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(seq_preview_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(seq_preview_probe PRIVATE cxx_std_20)
target_compile_definitions(seq_preview_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(seq_preview_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
