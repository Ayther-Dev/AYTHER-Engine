# ---------------------------------------------------------------------------
# preview_isolation_smoke — aislamiento de previews por el espejo (#338), ver main.cpp.
# Built only when -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(preview_isolation_smoke main.cpp)

target_link_libraries(preview_isolation_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(preview_isolation_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(preview_isolation_smoke PRIVATE cxx_std_20)

set_target_properties(preview_isolation_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
