# abi_frame_delta (E-6 #405) — el Frame Delta Stream del core sirve como
# invalidacion de VRAM si y solo si sus dirty_patterns son un SUPERCONJUNTO de
# lo que realmente cambio. Esto lo mide frame a frame contra la VRAM leida
# entera, y de paso compara el costo de los dos caminos. Necesita ROM y un core
# con ABI + FRAME_DELTA_V1.
add_executable(abi_frame_delta main.cpp)
target_link_libraries(abi_frame_delta PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(abi_frame_delta PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(abi_frame_delta PRIVATE cxx_std_20)
target_compile_definitions(abi_frame_delta PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(abi_frame_delta PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- ctest (#401) -----------------------------------------------------------
# Escrito contra el core `15872fc`, donde FALLABA con razon: el delta llegaba
# siempre vacio (0 patterns marcados en 240 frames contra 711 cambiados) y el
# raster journal estaba clavado en su tope de 256 desde el primer frame. Las dos
# causas estaban en el fork y se arreglaron ahi (#405):
#   · `update_bg_pattern_cache()` limpiaba `bg_name_dirty[name]=0` DENTRO de
#     `render_line_impl`, a mitad del frame → ahora el poll lee un espejo
#     acumulativo propio (`ayther_vram_dirty`) que solo vacia el propio poll;
#   · `ayther_raster_journal_count` no se reiniciaba en ningun lado → ahora si,
#     en `vdp_ayther_begin_frame()`, junto a `ayther_raster_dirty`.
# Con la ROM y el core del fork que haya EN LA MAQUINA (ninguno se commitea). Si
# faltan, el binario hace `skip` y sale 0 — el test pasa sin mentir sobre lo que
# cubrio. Un core viejo lo vuelve a poner en rojo, que es exactamente lo que
# tiene que pasar.
# #415: el registro colgaba de un if() sobre roms/, que esta en .gitignore — o
# sea que en una checkout limpia (CI incluida) este test NI SE CREABA. Ahora se
# registra SIEMPRE: ROM comercial si la maquina tiene una, y si no la SINTETICA
# que el binario se fabrica (tools/common/synth_rom.h).
ayther_abi_test_env(_abi_env)
add_test(NAME abi_frame_delta COMMAND abi_frame_delta)
set_tests_properties(abi_frame_delta PROPERTIES
    ENVIRONMENT      "${_abi_env}"
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration;abi")
