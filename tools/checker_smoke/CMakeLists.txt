# checker_smoke (#277) — modo UV checker + cobertura, verificado por pixel:
# celda sin mapear = checker ambar EXACTO (silueta respetada), anclaje AL
# elemento (tile x=8 continua el patron de 6px), autorado-sin-cargar = magenta,
# con asset = HD sin checker, cobertura numerica sube/baja con asignaciones.
# Requiere GPU (ventana SDL oculta). Built only when -DAYTHER_BUILD_SPIKE=ON.
add_executable(checker_smoke main.cpp)
target_link_libraries(checker_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(checker_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(checker_smoke PRIVATE cxx_std_20)
target_compile_definitions(checker_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(checker_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
