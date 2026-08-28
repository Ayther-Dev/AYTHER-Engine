cmake_minimum_required(VERSION 3.21)

project(AytherPackageConsumer LANGUAGES CXX)

find_package(Ayther 0.1 CONFIG REQUIRED COMPONENTS engine)

add_executable(ayther_package_consumer main.cpp)
target_compile_features(ayther_package_consumer PRIVATE cxx_std_20)
target_link_libraries(ayther_package_consumer PRIVATE Ayther::engine)
