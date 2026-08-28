# abi_write_control (E-4 #400) — las escrituras de control por write_control:
# READ_ONLY donde corresponde, efecto REAL sobre el framebuffer, generacion que
# avanza, y no-op limpio con core stock. Necesita ROM y un core con ABI.
add_executable(abi_write_control main.cpp)
target_link_libraries(abi_write_control PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(abi_write_control PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(abi_write_control PRIVATE cxx_std_20)
target_compile_definitions(abi_write_control PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(abi_write_control PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- ctest (#401) -----------------------------------------------------------
# Estos oraculos vivian fuera de ctest y solo corrian si alguien se acordaba de
# invocarlos a mano: por eso la regresion de sprites del viewport llego hasta la
# pantalla. Se registran con la ROM y el core del fork que haya EN LA MAQUINA
# (ninguno de los dos se commitea). Si faltan, el binario ya hace `skip` y sale
# 0 — el test pasa sin mentir sobre lo que cubrio.
# #415: el registro colgaba de un if() sobre roms/, que esta en .gitignore — o
# sea que en una checkout limpia (CI incluida) este test NI SE CREABA. Ahora se
# registra SIEMPRE: ROM comercial si la maquina tiene una, y si no la SINTETICA
# que el binario se fabrica (tools/common/synth_rom.h).
ayther_abi_test_env(_abi_env)
add_test(NAME abi_escrituras COMMAND abi_write_control)
set_tests_properties(abi_escrituras PROPERTIES
    ENVIRONMENT      "${_abi_env}"
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;abi")
