# ---------------------------------------------------------------------------
# hd_c8_slot_smoke — E4/C8: valida sprite_sub_slot (el slot SAT por sub que
# alimenta el z-order del renderer entre HD superpuestos). -DAYTHER_BUILD_SPIKE=ON.
# ---------------------------------------------------------------------------

add_executable(hd_c8_slot_smoke main.cpp)

target_link_libraries(hd_c8_slot_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(hd_c8_slot_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(hd_c8_slot_smoke PRIVATE cxx_std_20)
target_compile_definitions(hd_c8_slot_smoke PRIVATE
    AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_target_properties(hd_c8_slot_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
