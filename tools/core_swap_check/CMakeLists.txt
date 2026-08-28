# core_swap_check — evidencia para decidir si un core candidato puede reemplazar
# al que usa el Lab: corre la misma ROM los mismos frames con los dos binarios y
# compara lo que el Engine OBSERVA por los caminos legacy (RAM, framebuffer,
# sprites parseados). No entra al ctest: necesita ROM y dos DLLs concretas.
add_executable(core_swap_check main.cpp)
target_link_libraries(core_swap_check PRIVATE ayther_engine ayther_core)
if(WIN32)
    target_link_libraries(core_swap_check PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(core_swap_check PRIVATE cxx_std_20)
target_compile_definitions(core_swap_check PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(core_swap_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
