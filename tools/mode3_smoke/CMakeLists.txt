# ---------------------------------------------------------------------------
# mode3_smoke — valida el wiring de Modo 3 (RAM anchoring) en AytherSession:
# produce_frame captura la cámara del VDP, llama al Mode3Resolver y publica
# entity_instances/entity_subs en FrameView. Headless (sin Vulkan/ventana).
# Corre contra Sonic 2 + el core fork: bin/mode3_smoke <core_vram.dll> <rom>.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(mode3_smoke main.cpp)

target_link_libraries(mode3_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(mode3_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(mode3_smoke PRIVATE cxx_std_20)
target_compile_definitions(mode3_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(mode3_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
