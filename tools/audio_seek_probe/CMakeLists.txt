# audio_seek_probe — sonda de #490: ¿el audio ORIGINAL cambia de timbre al
# llegar a un frame por seek FRIO en vez de reproduciendo? Mide RMS y ZCR
# (proxy del centroide espectral) sobre la misma ventana.
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM via AYTHER_PROBE_ROM.
add_executable(audio_seek_probe main.cpp)
target_link_libraries(audio_seek_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(audio_seek_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(audio_seek_probe PRIVATE cxx_std_20)
target_compile_definitions(audio_seek_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(audio_seek_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
