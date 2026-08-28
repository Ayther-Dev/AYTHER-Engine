# sound_mailbox_probe — F1a de #507: encuentra la casilla donde el 68k deja el
# id de sonido para el Z80, por diferencial de work RAM alrededor del primer
# key-on de FM. Built only when -DAYTHER_BUILD_SPIKE=ON.
add_executable(sound_mailbox_probe main.cpp)
target_link_libraries(sound_mailbox_probe PRIVATE ayther_engine ayther_core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(sound_mailbox_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(sound_mailbox_probe PRIVATE cxx_std_20)
set_target_properties(sound_mailbox_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
