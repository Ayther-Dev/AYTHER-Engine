cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(AytherMainAudioTrial LANGUAGES CXX)
    enable_testing()
    if(NOT EXISTS "${QA_ENGINE_REFERENCE_SOURCE}/include/ayther/audio_player.h")
        message(FATAL_ERROR "QA_ENGINE_REFERENCE_SOURCE must identify the matching Engine source reference")
    endif()
    find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)
    set(qa_engine_include "${QA_ENGINE_REFERENCE_SOURCE}/include")
else()
    set(qa_engine_include "${PROJECT_SOURCE_DIR}/include")
endif()
find_package(SDL3 CONFIG REQUIRED)

add_executable(audio_qa_main_capture main_capture.cpp)
target_compile_features(audio_qa_main_capture PRIVATE cxx_std_20)
set_target_properties(audio_qa_main_capture PROPERTIES CXX_EXTENSIONS OFF)
# Engine-owned test: AudioPlayer is internal and is not a Runtime SDK contract.
target_include_directories(audio_qa_main_capture SYSTEM PRIVATE "${qa_engine_include}")
target_link_libraries(audio_qa_main_capture PRIVATE Ayther::engine SDL3::SDL3)
if(MSVC)
    target_compile_options(audio_qa_main_capture PRIVATE /W4 /WX /permissive- /EHsc)
else()
    target_compile_options(audio_qa_main_capture PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(COMMAND ayther_instrument_target)
    ayther_instrument_target(audio_qa_main_capture)
endif()

# Select exact published runtime dependencies when validating the pinned reference.
if(WIN32 AND QA_RUNTIME_BIN)
    add_custom_command(TARGET audio_qa_main_capture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${QA_RUNTIME_BIN}" "$<TARGET_FILE_DIR:audio_qa_main_capture>"
        VERBATIM)
endif()

foreach(mode IN ITEMS known silence auxiliary auxiliary_missing prime prime_missing conversion conversion_missing closing closing_discard)
    add_test(NAME audio_qa_main_capture_${mode}
        COMMAND ${CMAKE_COMMAND}
            "-DTRIAL=$<TARGET_FILE:audio_qa_main_capture>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/known-pcm-v1.s16le.pcm"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            "-DMODE=${mode}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/main_capture_test.cmake")
    set_tests_properties(audio_qa_main_capture_${mode} PROPERTIES
        TIMEOUT 15 LABELS "audio_qa;integration"
        ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_AUDIO_FREQUENCY=44100;SDL_AUDIO_CHANNELS=2;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=512")
endforeach()
