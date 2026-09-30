cmake_minimum_required(VERSION 3.25)
project(AytherReferenceOracle LANGUAGES CXX)
enable_testing()
set(header "${QA_ENGINE_REFERENCE_SOURCE}/include/ayther/audio_live_resume.h")
file(SHA256 "${header}" header_sha)
if(NOT header_sha STREQUAL "60c64ed5f4cecfb1119206c942f3590f9e3dd1aaabb2312f04b8126667d3ad26")
    message(FATAL_ERROR "Resume reference changed; oracle requires review")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header}")
add_executable(audio_qa_reference_decision reference_decision.cpp)
target_compile_features(audio_qa_reference_decision PRIVATE cxx_std_20)
set_target_properties(audio_qa_reference_decision PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(audio_qa_reference_decision PRIVATE /W4 /WX /permissive- /EHsc)
target_include_directories(audio_qa_reference_decision SYSTEM PRIVATE "${QA_ENGINE_REFERENCE_SOURCE}/include/ayther")
foreach(mode IN ITEMS resume invalid)
    add_test(NAME audio_qa_reference_decision_${mode}
        COMMAND ${CMAKE_COMMAND} "-DTRIAL=$<TARGET_FILE:audio_qa_reference_decision>"
            "-DFIXTURE=${QA_ORACLE_FIXTURE}" "-DMODE=${mode}"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            "-DSOURCE=${header}" "-DADAPTER=${CMAKE_CURRENT_SOURCE_DIR}/reference_decision.cpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake")
    set_tests_properties(audio_qa_reference_decision_${mode} PROPERTIES TIMEOUT 10 LABELS "audio_qa;reference")
endforeach()
