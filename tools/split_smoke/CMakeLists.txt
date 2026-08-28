# ---------------------------------------------------------------------------
# split_smoke — headless validation of AytherSession::split_recording (Fase C).
# Verifica estructura (inputs/CSR/trim rebasados) y determinismo del tail
# (estado de máquina byte-idéntico al original en el frame de corte).
#
# Built only when -DAYTHER_BUILD_SPIKE=ON. ROM/take via args (BYOR).
# ---------------------------------------------------------------------------

add_executable(split_smoke main.cpp)

target_link_libraries(split_smoke PRIVATE
    ayther_engine
    ayther_core
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(split_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()

target_compile_features(split_smoke PRIVATE cxx_std_20)

set_target_properties(split_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
