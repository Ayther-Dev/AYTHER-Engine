# abi_read_parity (E-3 #399) — el camino de lectura v1 devuelve LO MISMO que los
# punteros legacy, sobre el mismo frame. Es lo que justifica migrar los callers
# (y, en E-5, borrar los viejos). Necesita ROM y un core con ABI.
add_executable(abi_read_parity main.cpp)
target_link_libraries(abi_read_parity PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(abi_read_parity PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(abi_read_parity PRIVATE cxx_std_20)
target_compile_definitions(abi_read_parity PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(abi_read_parity PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
add_test(NAME abi_lecturas COMMAND abi_read_parity)
set_tests_properties(abi_lecturas PROPERTIES
    ENVIRONMENT      "${_abi_env}"
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;abi")
