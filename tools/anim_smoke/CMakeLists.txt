# ---------------------------------------------------------------------------
# anim_smoke — valida el wiring de Animaciones C-S2 (playback HD
# en fase, Pop y tween geometrico) a nivel AytherSession, contra Sonic 2.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(anim_smoke main.cpp)

target_link_libraries(anim_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(anim_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(anim_smoke PRIVATE cxx_std_20)
target_compile_definitions(anim_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(anim_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
