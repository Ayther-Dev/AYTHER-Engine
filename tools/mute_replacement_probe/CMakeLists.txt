# ---------------------------------------------------------------------------
# mute_replacement_probe — #329: el mute alcanza al REEMPLAZO (asset HD /
# SoundFont), no sólo a la voz original del chip. Ver main.cpp.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(mute_replacement_probe main.cpp)

target_link_libraries(mute_replacement_probe PRIVATE
    ayther_engine     # AytherSession + AudioPlayer (SDL3 PUBLIC: el probe abre
    ayther_core       # un device "dummy" para poder medir el CORTE de un asset)
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(mute_replacement_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(mute_replacement_probe PRIVATE cxx_std_20)

set_target_properties(mute_replacement_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
