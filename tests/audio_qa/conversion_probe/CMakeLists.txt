cmake_minimum_required(VERSION 3.25)
project(AytherConversionProbe LANGUAGES C CXX)
enable_testing()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS observer.h observer_impl.h)

if(NOT WIN32)
    message(FATAL_ERROR "This reference experiment currently pins the Windows distribution")
endif()
file(SHA256 "${QA_SDL_SOURCE}/src/audio/SDL_audiocvt.c" converter_hash)
file(SHA256 "${QA_SDL_SOURCE}/src/audio/SDL_audioresample.c" resampler_hash)
if(NOT converter_hash STREQUAL "4bfcf6cdab1af144dee9733770b1116a1859ba5d62294bbc4669278c468de9a4"
   OR NOT resampler_hash STREQUAL "3d21cb5aaafaa5e9795a83bc163b4df2048309620ce7a2953b6aa3f46030951d")
    message(FATAL_ERROR "SDL 3.4.8 source identity differs; review the experiment before rebasing")
endif()

# Never patch the dependency cache or the published reference in place.
set(probe_source "${CMAKE_CURRENT_BINARY_DIR}/sdl-observed-source")
file(COPY "${QA_SDL_SOURCE}/" DESTINATION "${probe_source}")
file(COPY observer.h observer_impl.h DESTINATION "${probe_source}/src/audio")
file(READ "${probe_source}/src/audio/SDL_audiocvt.c" converter)
string(REPLACE "\r\n" "\n" converter "${converter}")
string(REPLACE "#include \"SDL_audioresample.h\""
    "#include \"SDL_audioresample.h\"\n#include \"observer_impl.h\"" converter "${converter}")
string(REPLACE "    const Sint64 resample_rate = GetAudioStreamResampleRate(stream, src_spec->freq, stream->resample_offset);"
    "    const Sint64 qa_phase_before = stream->resample_offset;\n    const Sint64 resample_rate = GetAudioStreamResampleRate(stream, src_spec->freq, stream->resample_offset);" converter "${converter}")
string(REPLACE "        return true;\n    }\n\n    // Time to do some resampling!"
    "        AytherQaObserveConversion(stream, output_frames, output_frames, 0, resample_rate, qa_phase_before, gain, buf);\n        return true;\n    }\n\n    // Time to do some resampling!" converter "${converter}")
string(REPLACE "    return true;\n}\n\n// get converted/resampled data from the stream"
    "    AytherQaObserveConversion(stream, input_frames, output_frames, padding_frames, resample_rate, qa_phase_before, gain, buf);\n    return true;\n}\n\n// get converted/resampled data from the stream" converter "${converter}")
string(REPLACE "    if (retval) {\n        if (stream->put_callback) {"
    "    if (retval) {\n        AytherQaObserveSubmission(stream, spec, len, buf);\n        if (stream->put_callback) {" converter "${converter}")
string(REPLACE "    SDL_ClearAudioQueue(stream->queue);\n    SDL_zero(stream->input_spec);"
    "    const size_t qa_queued_before = SDL_GetAudioQueueQueued(stream->queue);\n    const Sint64 qa_reset_phase_before = stream->resample_offset;\n    SDL_ClearAudioQueue(stream->queue);\n    SDL_zero(stream->input_spec);" converter "${converter}")
string(REPLACE "    stream->resample_offset = 0;\n\n    SDL_UnlockMutex(stream->lock);\n    return true;\n}\n\nvoid SDL_DestroyAudioStream"
    "    stream->resample_offset = 0;\n    AytherQaObserveReset(stream, qa_queued_before, qa_reset_phase_before);\n\n    SDL_UnlockMutex(stream->lock);\n    return true;\n}\n\nvoid SDL_DestroyAudioStream" converter "${converter}")
string(REPLACE "    SDL_aligned_free(stream->work_buffer);\n    SDL_DestroyAudioQueue(stream->queue);"
    "    AytherQaObserveEnd(stream);\n    SDL_aligned_free(stream->work_buffer);\n    SDL_DestroyAudioQueue(stream->queue);" converter "${converter}")
file(WRITE "${probe_source}/src/audio/SDL_audiocvt.c" "${converter}")
file(READ "${probe_source}/src/audio/SDL_audioresample.c" resampler)
string(REPLACE "\r\n" "\n" resampler "${resampler}")
string(REPLACE "int SDL_GetResamplerPaddingFrames(Sint64 resample_rate)"
    "/* AYTHER QA altered-source marker: expose actual compiled filter support. */\nint AytherQaResamplerSupportLeft(void) { return RESAMPLER_ZERO_CROSSINGS - 1; }\nint AytherQaResamplerSupportRight(void) { return RESAMPLER_ZERO_CROSSINGS; }\n\nint SDL_GetResamplerPaddingFrames(Sint64 resample_rate)"
    resampler "${resampler}")
file(WRITE "${probe_source}/src/audio/SDL_audioresample.c" "${resampler}")

set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${probe_source}" sdl-build EXCLUDE_FROM_ALL)

if(QA_ENGINE_REFERENCE_SOURCE)
    find_package(Ayther 0.1.0 CONFIG REQUIRED COMPONENTS engine)
    add_executable(audio_qa_common_mix common_mix.cpp)
    target_compile_features(audio_qa_common_mix PRIVATE cxx_std_20)
    target_compile_options(audio_qa_common_mix PRIVATE /W4 /WX /permissive- /EHsc)
    target_include_directories(audio_qa_common_mix SYSTEM PRIVATE "${QA_ENGINE_REFERENCE_SOURCE}/include")
    target_link_libraries(audio_qa_common_mix PRIVATE Ayther::engine SDL3::SDL3)
    set_target_properties(audio_qa_common_mix PROPERTIES CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/common-mix")
    add_custom_command(TARGET audio_qa_common_mix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QA_RUNTIME_BIN}" "$<TARGET_FILE_DIR:audio_qa_common_mix>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3-shared>"
            "$<TARGET_FILE_DIR:audio_qa_common_mix>/SDL3.dll" VERBATIM)
    foreach(mode IN ITEMS all gain muted main_only missing_contribution drain drain_loss prime_resample prime_missing prime_loss gain_switch mute_switch transport_cut previews_cut natural_end gain_switch_loss)
        add_test(NAME audio_qa_common_mix_${mode}
            COMMAND ${CMAKE_COMMAND} "-DTRIAL=$<TARGET_FILE:audio_qa_common_mix>"
                "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/../data/known-pcm-v1.s16le.pcm"
                "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
                "-DMODE=${mode}" -P "${CMAKE_CURRENT_SOURCE_DIR}/common_mix_test.cmake")
        set(output_rate 44100)
        if(mode MATCHES "^prime_")
            set(output_rate 48000)
        endif()
        set_tests_properties(audio_qa_common_mix_${mode} PROPERTIES TIMEOUT 15 LABELS "audio_qa;integration"
            ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_AUDIO_FREQUENCY=${output_rate};SDL_AUDIO_CHANNELS=2;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=512")
    endforeach()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../staging_probe" staging-probe)
endif()

foreach(kind IN ITEMS observed reference)
    add_executable(audio_qa_conversion_${kind} conversion_probe.cpp)
    target_compile_features(audio_qa_conversion_${kind} PRIVATE cxx_std_20)
    set_target_properties(audio_qa_conversion_${kind} PROPERTIES CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${kind}")
    target_compile_options(audio_qa_conversion_${kind} PRIVATE /W4 /WX /permissive- /EHsc)
    target_link_libraries(audio_qa_conversion_${kind} PRIVATE SDL3::SDL3)
    if(kind STREQUAL "observed")
        target_compile_definitions(audio_qa_conversion_${kind} PRIVATE QA_EXPECT_OBSERVER=1)
        add_custom_command(TARGET audio_qa_conversion_${kind} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3-shared>"
                "$<TARGET_FILE_DIR:audio_qa_conversion_${kind}>/SDL3.dll" VERBATIM)
    else()
        target_compile_definitions(audio_qa_conversion_${kind} PRIVATE QA_EXPECT_OBSERVER=0)
        add_custom_command(TARGET audio_qa_conversion_${kind} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QA_RUNTIME_BIN}/SDL3.dll"
                "$<TARGET_FILE_DIR:audio_qa_conversion_${kind}>/SDL3.dll" VERBATIM)
    endif()
endforeach()

foreach(fault IN ITEMS drop phase reset_drop reset_phase)
    set(fault_mode resample)
    if(fault MATCHES "^reset_")
        set(fault_mode reset)
    endif()
    add_test(NAME audio_qa_conversion_reject_${fault}
        COMMAND ${CMAKE_COMMAND}
            "-DOBSERVED=$<TARGET_FILE:audio_qa_conversion_observed>"
            "-DREFERENCE=$<TARGET_FILE:audio_qa_conversion_reference>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/../data/known-pcm-v1.s16le.pcm"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            "-DMODE=${fault_mode}" "-DFAULT=${fault}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake")
    set_tests_properties(audio_qa_conversion_reject_${fault} PROPERTIES
        TIMEOUT 15 LABELS "audio_qa;integration")
endforeach()

foreach(mode IN ITEMS numeric resample drc reset)
    add_test(NAME audio_qa_conversion_phase_${mode}
        COMMAND ${CMAKE_COMMAND}
            "-DOBSERVED=$<TARGET_FILE:audio_qa_conversion_observed>"
            "-DREFERENCE=$<TARGET_FILE:audio_qa_conversion_reference>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/../data/known-pcm-v1.s16le.pcm"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            "-DMODE=${mode}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake")
    set_tests_properties(audio_qa_conversion_phase_${mode} PROPERTIES
        TIMEOUT 15 LABELS "audio_qa;integration")
endforeach()
