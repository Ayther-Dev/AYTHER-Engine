# overlay_vscroll_smoke (#484) — la lane Custom bajo vscroll POR COLUMNA (modo VS
# del VDP) EJECUTADA en la GPU y comparada por píxel contra su fórmula:
# fila_lámina = (y + v_col(x/16)) mod img_h. Sin ROM ni grabación: una lámina
# sintética que codifica fila/columna en el color, FrameView armado a mano
# (vs_two_cell + plane_vscroll_col) y un stack con solo la capa Custom.
# Requiere GPU (ventana SDL oculta) — detrás de AYTHER_BUILD_SPIKE, como el
# resto de los oráculos de readback (molde: sprite_mask_smoke).
add_executable(overlay_vscroll_smoke main.cpp)
target_link_libraries(overlay_vscroll_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(overlay_vscroll_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(overlay_vscroll_smoke PRIVATE cxx_std_20)
target_compile_definitions(overlay_vscroll_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(overlay_vscroll_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Entra al ctest: solo necesita GPU, mismo criterio que sprite_mask.
add_test(NAME acetato_vs COMMAND overlay_vscroll_smoke)
set_tests_properties(acetato_vs PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
