# element_group_hide_smoke (2026-07-31) — ocultar un miembro DESACTIVA el asset
# del grupo: un asset HD reemplaza a una pose/objeto entero, así que el ojo del
# Inspector tiene que apagarlo y dejar ver los miembros que siguen visibles. Fija
# también que el ancla conserva su `sub` (la lane plana no lo re-dibuja) y que no
# hay contagio entre dos poses solapadas. Built only when -DAYTHER_BUILD_SPIKE=ON.
# Args: <poses.toml> <rec.arp> [f0] [f1] · ROM via AYTHER_PROBE_ROM.
add_executable(element_group_hide_smoke main.cpp)
target_link_libraries(element_group_hide_smoke PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(element_group_hide_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(element_group_hide_smoke PRIVATE cxx_std_20)
target_compile_definitions(element_group_hide_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(element_group_hide_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
