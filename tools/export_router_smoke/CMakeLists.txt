# ---------------------------------------------------------------------------
# export_router_smoke — el export rinde por el router (#331), ver main.cpp.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(export_router_smoke main.cpp)

target_link_libraries(export_router_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(export_router_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(export_router_smoke PRIVATE cxx_std_20)

set_target_properties(export_router_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
