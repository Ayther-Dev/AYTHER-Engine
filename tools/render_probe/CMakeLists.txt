# render_probe (spec 002, plan §5.13) - reproduces a take without a window
# with the full pipeline (session + Vulkan renderer) and writes, per frame,
# the composed image, the core's framebuffer, the render observation and the
# O1/O2 checks. Needs a GPU (hidden SDL window).
add_executable(render_probe render_probe.cpp)
target_link_libraries(render_probe PRIVATE
    ayther_engine
    ayther_core
    SDL3::SDL3
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(render_probe PRIVATE ws2_32 userenv bcrypt ntdll)
endif()
target_include_directories(render_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/include/ayther"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_features(render_probe PRIVATE cxx_std_20)
target_compile_definitions(render_probe PRIVATE
    AYTHER_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(render_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
