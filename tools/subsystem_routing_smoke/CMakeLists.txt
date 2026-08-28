# ---------------------------------------------------------------------------
# subsystem_routing_smoke — #292: el routing original/HD por subsistema.
#
# En el ctest normal: usa la ROM sintética del repo y no pide GPU (mide la
# decisión del motor, no el pixel). Sin el core del fork se declara SALTEADO
# (77) en vez de pasar en verde sin haber medido nada.
# ---------------------------------------------------------------------------
add_executable(subsystem_routing_smoke main.cpp)
target_link_libraries(subsystem_routing_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(subsystem_routing_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(subsystem_routing_smoke PRIVATE cxx_std_20)
target_compile_definitions(subsystem_routing_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(subsystem_routing_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME subsystem_routing COMMAND subsystem_routing_smoke)
set_tests_properties(subsystem_routing PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT          120
    LABELS           "engine;native;integration")
