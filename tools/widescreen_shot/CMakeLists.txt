# widescreen_shot (#231 / #421) — arma la Panorámica headless (mismo protocolo
# que «Crear Panorámica» del Lab) y renderiza el MISMO frame nativo y ensanchado
# a dos PNG. Es la verificación que ninguna de las dos épicas podía automatizar:
# que el arte lateral continúe el del centro se mira, no se afirma.
# Requiere GPU (ventana SDL oculta). ROM via AYTHER_PROBE_ROM.
add_executable(widescreen_shot main.cpp)
target_link_libraries(widescreen_shot PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(widescreen_shot PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(widescreen_shot PRIVATE cxx_std_20)
target_compile_definitions(widescreen_shot PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(widescreen_shot PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
