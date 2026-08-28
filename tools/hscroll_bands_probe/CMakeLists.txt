# ---------------------------------------------------------------------------
# hscroll_bands_probe (#421) — mide si un plano tiene BANDAS con scroll
# horizontal independiente (line-scroll) o si se desplaza entero. Es el paso
# previo que el issue exige antes de disenar nada: la Panoramica modela UNA
# camara por tira, y hay que saber si el caso ocurre de verdad y en que plano.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(hscroll_bands_probe main.cpp)

target_link_libraries(hscroll_bands_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(hscroll_bands_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(hscroll_bands_probe PRIVATE cxx_std_20)
target_compile_definitions(hscroll_bands_probe PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(hscroll_bands_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
