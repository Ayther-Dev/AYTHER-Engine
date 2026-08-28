# snapshot_mask_smoke (2026-08-06) — las capas elegidas en el diálogo Snapshot
# llegan al PNG: el vdp_mask de export_frame se verifica por píxel (neutralidad
# byte-exacta con todas las capas, efecto NO vacuo al apagar una, acumulación).
# Requiere GPU (ventana SDL oculta).
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(snapshot_mask_smoke main.cpp)
target_link_libraries(snapshot_mask_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(snapshot_mask_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(snapshot_mask_smoke PRIVATE cxx_std_20)
target_compile_definitions(snapshot_mask_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(snapshot_mask_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
