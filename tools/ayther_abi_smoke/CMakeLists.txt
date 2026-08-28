# ayther_abi_smoke (E-1 #397) — la negociacion de la ABI AYTHER v1, en los DOS
# casos: con el fork negocia y declara capabilities; con un core STOCK la carga
# sigue funcionando y has_ayther_v1() da false (la negociacion es aditiva y no
# puede romper a nadie). Necesita una ROM (AYTHER_PROBE_ROM) y, para el caso
# positivo, la DLL del fork (AYTHER_ABI_CORE) — que no vive en este repo.
add_executable(ayther_abi_smoke main.cpp)
target_link_libraries(ayther_abi_smoke PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(ayther_abi_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(ayther_abi_smoke PRIVATE cxx_std_20)
target_compile_definitions(ayther_abi_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(ayther_abi_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- ctest (#401, #415) ------------------------------------------------------
# Estos oraculos vivian fuera de ctest y solo corrian si alguien se acordaba de
# invocarlos a mano: por eso la regresion de sprites del viewport llego hasta la
# pantalla.
#
# #415: el registro estaba detras de un if() sobre roms/, que esta en .gitignore
# — o sea que en una checkout limpia (CI incluida) el test NI SE CREABA, y el
# 46/46 local era 41/41 alla. Ahora se registra SIEMPRE: si la maquina tiene una
# ROM comercial se usa esa (mide mas), y si no, el binario se fabrica la
# SINTETICA (tools/common/synth_rom.h).
ayther_abi_test_env(_abi_env)
add_test(NAME abi_negociacion COMMAND ayther_abi_smoke)
set_tests_properties(abi_negociacion PROPERTIES
    ENVIRONMENT      "${_abi_env}"
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;abi")
