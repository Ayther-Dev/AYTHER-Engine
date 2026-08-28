# segacd_pcm_probe (#409) — mide si los eventos del chip PCM de Sega CD llegan
# al frontend por poll_audio_events, y si el event_size del core coincide con el
# struct local (el gotcha de la union en ayther_audio_event_v1). Necesita una
# ISO de Sega CD y el BIOS: sin BIOS el core no monta el disco.
add_executable(segacd_pcm_probe main.cpp)
target_link_libraries(segacd_pcm_probe PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(segacd_pcm_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(segacd_pcm_probe PRIVATE cxx_std_20)
target_compile_definitions(segacd_pcm_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(segacd_pcm_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Sin add_test: depende de una ISO y de un BIOS que NO se commitean.
#   AYTHER_PROBE_ROM=<iso> AYTHER_SYSTEM_DIR=<dir con bios_CD_U.bin> \
#     build/bin/segacd_pcm_probe
