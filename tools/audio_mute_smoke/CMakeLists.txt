# ---------------------------------------------------------------------------
# audio_mute_smoke — valida el mute selectivo por canal (C-A3): mide la energía
# del PCM emitido bajo distintas máscaras de mute (id 0x10D del fork) desde un
# mismo savestate, y el determinismo del PCM. Sólo RetroRunner (sin core Rust).
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(audio_mute_smoke
    smoke.cpp
    ${CMAKE_SOURCE_DIR}/src/retro_runner.cpp
    ${CMAKE_SOURCE_DIR}/src/log.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime_options.cpp
)

target_include_directories(audio_mute_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include/ayther
    ${CMAKE_SOURCE_DIR}/tools/common          # synth_rom.h (#415)
)
target_compile_definitions(audio_mute_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_compile_features(audio_mute_smoke PRIVATE cxx_std_20)

set_target_properties(audio_mute_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


# #415: al ctest de todos los dias. Sin el core del fork se declara SALTEADO
# (77) en vez de pasar en verde sin haber medido nada — el modo de fallo que
# tuvo a este repo dos dias con el audio mudo y ctest en 45/45.
add_test(NAME audio_mute COMMAND audio_mute_smoke)
set_tests_properties(audio_mute PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;audio")

# #230 EM-7.4: `RetroRunner` aplica el parche IPS/BPS del usuario, que vive
# en el core. Este smoke no lo usa, pero el simbolo tiene que resolver: un
# enlace incompleto no es un test que se saltea, es un binario que no existe.
target_link_libraries(audio_mute_smoke PRIVATE ayther_core)
