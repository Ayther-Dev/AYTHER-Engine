# ---------------------------------------------------------------------------
# background_smoke — valida el wiring de Fondos (captura del stitcher +
# export de PNG por capa) a nivel AytherSession, contra Sonic 2 real.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(background_smoke main.cpp)

target_link_libraries(background_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(background_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(background_smoke PRIVATE cxx_std_20)
target_compile_definitions(background_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(background_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
