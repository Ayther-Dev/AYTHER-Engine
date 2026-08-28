# widescreen_sprites_smoke (#231 EM-8.3) — un sprite que TOCA la pantalla nativa
# se dibuja entero en el area extendida; el que esta entero afuera (el idiom de
# estacionar sprites fuera de pantalla) NO aparece. Mide el fenomeno sobre una
# toma real y despues verifica la regla por pixel sobre una escena sintetica.
# Requiere GPU (ventana SDL oculta). Toma opcional; ROM via AYTHER_PROBE_ROM.
add_executable(widescreen_sprites_smoke main.cpp)
target_link_libraries(widescreen_sprites_smoke PRIVATE
    ayther_engine ayther_core SDL3::SDL3 Vulkan::Vulkan ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(widescreen_sprites_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(widescreen_sprites_smoke PRIVATE cxx_std_20)
target_compile_definitions(widescreen_sprites_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(widescreen_sprites_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Entra al ctest: la parte sintetica no necesita ROM ni grabacion, solo GPU.
add_test(NAME widescreen_sprites COMMAND widescreen_sprites_smoke)
set_tests_properties(widescreen_sprites PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
