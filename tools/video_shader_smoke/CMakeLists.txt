# video_shader_smoke (#378) — la conversion I420->RGB del fragment shader
# EJECUTADA en la GPU y comparada por pixel contra ayther::video_i420_to_bgra_px,
# que es su especificacion. Sin ROM ni grabacion: arma la FrameView a mano con
# tres planos sinteticos y deja visible solo la capa Video.
# Requiere GPU (ventana SDL oculta) — por eso vive detras de AYTHER_BUILD_SPIKE,
# como el resto de los oraculos de readback.
add_executable(video_shader_smoke main.cpp)
target_link_libraries(video_shader_smoke PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(video_shader_smoke PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_compile_features(video_shader_smoke PRIVATE cxx_std_20)
target_compile_definitions(video_shader_smoke PRIVATE AYTHER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(video_shader_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Entra al ctest: a diferencia de los otros oraculos de readback no necesita ROM
# ni grabacion, asi que corre solo. Lo unico que pide es una GPU — y AYTHER_BUILD_SPIKE
# ya es un flag de maquina de desarrollo.
add_test(NAME video_shader COMMAND video_shader_smoke)

# Necesita una GPU de verdad (crea un VkContext), asi que en una maquina sin
# Vulkan —los runners de CI, por ejemplo— hay que poder dejarlo afuera:
# ctest -LE gpu corre todo lo demas. Etiquetar es preferible a excluir por
# nombre, que se desactualiza en silencio cuando se agrega el siguiente.
set_tests_properties(video_shader PROPERTIES
    TIMEOUT 120
    LABELS  "engine;native;gpu")
