file(READ "${SOURCE_FILE}" source_text)
file(READ "${CMAKE_FILE}" cmake_text)

if(source_text MATCHES "__FILE__")
    message(FATAL_ERROR
        "Release tests must not derive fixture paths from __FILE__; "
        "release builds remap source paths for reproducibility.")
endif()

if(NOT source_text MATCHES "AYTHER_RF18_SCHEMA1_FIXTURE")
    message(FATAL_ERROR "elements_toml_test must use the injected RF-18 fixture path")
endif()

if(NOT cmake_text MATCHES "AYTHER_RF18_SCHEMA1_FIXTURE")
    message(FATAL_ERROR "integration CMake must inject the RF-18 fixture path")
endif()
