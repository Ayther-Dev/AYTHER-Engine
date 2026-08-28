# ---------------------------------------------------------------------------
# pano_anchor_probe — anclaje de Panorámica: paso a paso (produce cada frame)
# vs playback con catch-up (bare + produce del visible), para cazar diferencias
# de reconocimiento entre ambos caminos (reporte transición f103-f155).
#
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM/toma por argumentos (BYOR).
# ---------------------------------------------------------------------------

add_executable(pano_anchor_probe main.cpp)

target_link_libraries(pano_anchor_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(pano_anchor_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(pano_anchor_probe PRIVATE cxx_std_20)

set_target_properties(pano_anchor_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
