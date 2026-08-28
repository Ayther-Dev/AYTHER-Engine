# ---------------------------------------------------------------------------
# voice_router_smoke — oráculo del router por voz (#327). Ver main.cpp.
#
# Es SINTÉTICO (log de escrituras a mano, sin core ni ROM), así que entra al
# ctest normal y no depende de los juegos del usuario.
# ---------------------------------------------------------------------------

add_executable(voice_router_smoke main.cpp)

target_link_libraries(voice_router_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(voice_router_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(voice_router_smoke PRIVATE cxx_std_20)

set_target_properties(voice_router_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME voice_router COMMAND voice_router_smoke)
set_tests_properties(voice_router PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;integration;audio")
