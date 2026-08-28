# ---------------------------------------------------------------------------
# maper_probe — sonda headless del Maper (plan lab-memory-map-plan.md §4).
# v0: --peek/--watch (spike byte-order); M4 suma los experimentos canónicos.
#
# Built only when -DAYTHER_BUILD_SPIKE=ON. Core/ROM via args (BYOR).
# ---------------------------------------------------------------------------

add_executable(maper_probe main.cpp)

target_link_libraries(maper_probe PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(maper_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(maper_probe PRIVATE cxx_std_20)

set_target_properties(maper_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
