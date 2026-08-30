cmake_minimum_required(VERSION 3.21)

# The core-only consumer. It deliberately does NOT ask for the engine component:
# this is the build an embedder gets from the ayther-core artifact, which ships
# no renderer, no shaders, and no SDL3/Vulkan dependency surface. If this ever
# needs a native dependency to link, the core package has stopped being
# core-only and the release scope is wrong.
project(AytherCorePackageConsumer LANGUAGES CXX)

find_package(Ayther 0.1 CONFIG REQUIRED)

add_executable(ayther_core_package_consumer main.cpp)
target_compile_features(ayther_core_package_consumer PRIVATE cxx_std_20)
target_link_libraries(ayther_core_package_consumer PRIVATE Ayther::core)
