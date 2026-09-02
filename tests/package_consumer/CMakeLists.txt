cmake_minimum_required(VERSION 3.21)

project(AytherPackageConsumer LANGUAGES CXX)

find_package(Ayther 0.1 CONFIG REQUIRED COMPONENTS engine)

if(NOT DEFINED Ayther_SHADER_DIR OR Ayther_SHADER_DIR STREQUAL "")
    message(FATAL_ERROR
        "The installed engine package did not export Ayther_SHADER_DIR")
endif()
if(NOT IS_ABSOLUTE "${Ayther_SHADER_DIR}")
    message(FATAL_ERROR
        "Ayther_SHADER_DIR is not absolute: ${Ayther_SHADER_DIR}")
endif()

set(_ayther_required_shaders
    indexed_plane.frag.spv
    indexed_plane.vert.spv
    sprite.frag.spv
    sprite.vert.spv
    sprite_mask.frag.spv
    sprite_mult.frag.spv
    sprite_screen.frag.spv
    video.frag.spv)
foreach(_ayther_shader IN LISTS _ayther_required_shaders)
    if(NOT EXISTS "${Ayther_SHADER_DIR}/${_ayther_shader}")
        message(FATAL_ERROR
            "Installed Engine shader is missing: "
            "${Ayther_SHADER_DIR}/${_ayther_shader}")
    endif()
endforeach()

add_executable(ayther_package_consumer main.cpp)
target_compile_features(ayther_package_consumer PRIVATE cxx_std_20)
target_link_libraries(ayther_package_consumer PRIVATE Ayther::engine)
