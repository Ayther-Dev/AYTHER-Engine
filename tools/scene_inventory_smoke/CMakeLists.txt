# scene_inventory_smoke (#272) — el inventario de escena reproduce la escena:
# compone la pantalla en CPU SOLO desde los elementos de scene_inventory (con
# la semántica del VDP: buffer de sprites primero-gana por cadena + merge por
# prioridades) y compara bit a bit contra ayther_recompose_frame(base) — el
# mismo oráculo de R-1. Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via
# AYTHER_PROBE_ROM.
add_executable(scene_inventory_smoke main.cpp)
target_link_libraries(scene_inventory_smoke PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(scene_inventory_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(scene_inventory_smoke PRIVATE cxx_std_20)
target_compile_definitions(scene_inventory_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(scene_inventory_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
