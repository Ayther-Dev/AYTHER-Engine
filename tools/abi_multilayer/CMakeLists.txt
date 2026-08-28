# abi_multilayer (E-7 #406) — la extraccion multicapa del core, medida antes de
# construirle el cableado encima: composite bit-exacto contra el frame real en
# los frames sin actividad raster, capas que de verdad se separan, y la
# verificacion de que recomponer no altera la emulacion. Necesita ROM y un core
# con ABI + el export ayther_recompose_multilayer.
add_executable(abi_multilayer main.cpp)
target_link_libraries(abi_multilayer PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(abi_multilayer PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(abi_multilayer PRIVATE cxx_std_20)
target_compile_definitions(abi_multilayer PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(abi_multilayer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- ctest (#401) -----------------------------------------------------------
# Escrito contra un core donde fallaba 3 de 14, y las tres tenian razon:
#   · `ayther_recompose_multilayer` CRASHEABA el proceso con -O2 (`cram` leido
#     con movaps sin estar alineado a 16) — fork #21;
#   · los AYTHER_RC_ERR_* llegaban APLASTADOS a UNSUPPORTED, asi que «no es modo
#     5», «interlace doble», «filtro NTSC» y «parametros invalidos» eran
#     indistinguibles, y el criterio de #406 pide el motivo en el log — fork #22;
#   · no habia cache: el mismo frame dos veces costaba lo mismo — fork #23.
# Las tres arregladas en el fork. Con la ROM y el core que haya EN LA MAQUINA;
# si faltan, el binario hace `skip` y sale 0. Un core viejo lo vuelve a poner en
# rojo, que es exactamente lo que tiene que pasar.
# #415: el registro colgaba de un if() sobre roms/, que esta en .gitignore — o
# sea que en una checkout limpia (CI incluida) este test NI SE CREABA. Ahora se
# registra SIEMPRE: ROM comercial si la maquina tiene una, y si no la SINTETICA
# que el binario se fabrica (tools/common/synth_rom.h).
ayther_abi_test_env(_abi_env)
add_test(NAME abi_multilayer COMMAND abi_multilayer)
set_tests_properties(abi_multilayer PROPERTIES
    ENVIRONMENT      "${_abi_env}"
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;abi")
