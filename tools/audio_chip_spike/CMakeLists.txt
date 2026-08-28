# ---------------------------------------------------------------------------
# audio_chip_spike — harness headless que valida el riesgo #1 del workspace
# Audios (hito C-A1): que las escrituras a los chips de sonido (YM2612/SN76489)
# expuestas por el fork (ids 0x109/0x10A) sean replay-estables. Compila
# RetroRunner + core_loader (header) — sin SDL/Vulkan.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(audio_chip_spike
    spike.cpp
    ${CMAKE_SOURCE_DIR}/src/retro_runner.cpp
)

target_include_directories(audio_chip_spike PRIVATE
    ${CMAKE_SOURCE_DIR}/include/ayther
)

target_compile_features(audio_chip_spike PRIVATE cxx_std_20)

set_target_properties(audio_chip_spike PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# #230 EM-7.4: `RetroRunner` aplica el parche IPS/BPS del usuario, que vive
# en el core. Este smoke no lo usa, pero el simbolo tiene que resolver: un
# enlace incompleto no es un test que se saltea, es un binario que no existe.
target_link_libraries(audio_chip_spike PRIVATE ayther_core)
