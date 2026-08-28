# psg_identity_probe (E-8 #407) — mide, sobre musica real, si el PSG ofrece
# algo que permita separar dos tonos por «instrumento». Necesita ROM y el core
# del fork (lee FrameView.chip_writes).
add_executable(psg_identity_probe main.cpp)
target_link_libraries(psg_identity_probe PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(psg_identity_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(psg_identity_probe PRIVATE cxx_std_20)
target_compile_definitions(psg_identity_probe PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(psg_identity_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Sin add_test: es un probe de DIAGNOSTICO sobre musica de un juego concreto,
# no un invariante. Su resultado se lee, no se verifica en CI.
#   AYTHER_PROBE_ROM=<rom> build/bin/psg_identity_probe
