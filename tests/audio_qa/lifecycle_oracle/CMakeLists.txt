cmake_minimum_required(VERSION 3.25)
project(AytherLifecycleOracle LANGUAGES CXX)
enable_testing()
set(header "${QA_ENGINE_REFERENCE_SOURCE}/include/ayther/audio_hd_mixer.h")
file(SHA256 "${header}" identity)
if(NOT identity STREQUAL "f47f24ca73632799da944f2ec5b8425221c2deda475291f1c9263fd02448dae8")
    message(FATAL_ERROR "Lifecycle mixer reference changed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header}")
file(READ "${header}" mixer)
string(REPLACE "\r\n" "\n" mixer "${mixer}")
function(observe anchor replacement)
    string(FIND "${mixer}" "${anchor}" found)
    if(found LESS 0)
        message(FATAL_ERROR "Lifecycle observation anchor missing: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" mixer "${mixer}")
    set(mixer "${mixer}" PARENT_SCOPE)
endfunction()
observe("    struct Voice {" "    struct Voice {\n        uint64_t qa_occurrence = 0;")
observe("        voices_.push_back(std::move(v));" "        v.qa_occurrence = started_ + 1;\n        voices_.push_back(std::move(v));")
observe("                v.fade_span = kFadeFrames;   // the cut one, not the authored one"
    "                v.fade_span = kFadeFrames;   // the cut one, not the authored one\n                qa_lifecycle::event({\"key_stop_fade\", v.qa_occurrence, v.key, qa_lifecycle::frame, qa_lifecycle::boundary, v.pos, v.pos});")
observe("            bool done = false;"
    "            bool done = false;\n            const char* qa_end_reason = nullptr;\n            uint64_t qa_end_output = block_start;")
observe("                    if (!v.looping) { done = true; break; }"
    "                    if (!v.looping) { done = true; qa_end_reason = \"asset_end\"; qa_end_output = block_start + f; break; }")
observe("                    if (--v.fade_left == 0) done = true;"
    "                    if (--v.fade_left == 0) { done = true; qa_end_reason = \"fade_complete\"; }")
observe("                if (v.pos >= cycle_end) {" "                const size_t qa_before = v.pos;\n                if (v.pos >= cycle_end) {")
observe("                    v.pos = cycle_beg;"
    "                    v.pos = cycle_beg;\n                    qa_lifecycle::event({\"loop_wrap\", v.qa_occurrence, v.key, qa_lifecycle::frame, block_start + f, qa_before, v.pos});")
observe("                ++v.pos;"
    "                ++v.pos;\n                qa_end_output = block_start + f + 1;\n                qa_lifecycle::sample({v.qa_occurrence, v.key, block_start + f, qa_before, s / 2, v.pos, g, v.fade_left});")
observe("                         v.fade_left == 0 && v.start_sample <= block_start))\n                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));"
    "                         v.fade_left == 0 && v.start_sample <= block_start)) {\n                qa_lifecycle::event({qa_end_reason ? qa_end_reason : \"asset_end\", v.qa_occurrence, v.key, qa_lifecycle::frame, qa_end_output, v.pos, v.pos});\n                voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));\n            }")
observe("                if (v.cut_frame == UINT64_MAX || v.cut_frame <= v.end_frame)\n                    voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));\n                else"
    "                if (v.cut_frame == UINT64_MAX || v.cut_frame <= v.end_frame) {\n                    qa_lifecycle::event({\"window_end\", v.qa_occurrence, v.key, frame, qa_lifecycle::boundary, v.pos, v.pos});\n                    voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));\n                } else")
observe("            if (frame > v.cut_frame) {"
    "            if (frame > v.cut_frame) {\n                qa_lifecycle::event({\"cut_frame\", v.qa_occurrence, v.key, frame, qa_lifecycle::boundary, v.pos, v.pos});")
observe("            // FADE_OUT. On passing the limit the voice does not die abruptly:"
    "            qa_lifecycle::context({v.qa_occurrence, v.key, frame, qa_lifecycle::boundary, v.end_frame, v.cut_frame, v.fade_frames, v.fade_left, v.looping, v.pos});\n            // FADE_OUT. On passing the limit the voice does not die abruptly:")
observe("                    v.fade_span = v.fade_frames;"
    "                    v.fade_span = v.fade_frames;\n                    qa_lifecycle::event({\"authored_fade_begin\", v.qa_occurrence, v.key, frame, qa_lifecycle::boundary, v.pos, v.pos});")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/observed_mixer.h" "// AYTHER QA observation copy.\n${mixer}")
foreach(kind IN ITEMS reference observed)
    add_executable(audio_qa_lifecycle_${kind} lifecycle_probe.cpp)
    target_compile_features(audio_qa_lifecycle_${kind} PRIVATE cxx_std_20)
    target_compile_options(audio_qa_lifecycle_${kind} PRIVATE /W4 /WX /permissive- /EHsc)
    set_target_properties(audio_qa_lifecycle_${kind} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(audio_qa_lifecycle_${kind} SYSTEM PRIVATE
        "${QA_ENGINE_REFERENCE_SOURCE}/include/ayther" "${CMAKE_CURRENT_BINARY_DIR}")
    if(kind STREQUAL "observed")
        target_compile_definitions(audio_qa_lifecycle_${kind} PRIVATE QA_LIFECYCLE_OBSERVER=1)
    else()
        target_compile_definitions(audio_qa_lifecycle_${kind} PRIVATE QA_LIFECYCLE_OBSERVER=0)
    endif()
endforeach()
foreach(mode IN ITEMS loop window lost replacement replacement_lost natural natural_lost simultaneous simultaneous_lost)
    add_test(NAME audio_qa_lifecycle_${mode}
        COMMAND ${CMAKE_COMMAND} "-DREFERENCE=$<TARGET_FILE:audio_qa_lifecycle_reference>"
            "-DOBSERVED=$<TARGET_FILE:audio_qa_lifecycle_observed>" "-DMODE=${mode}"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake")
    set_tests_properties(audio_qa_lifecycle_${mode} PROPERTIES TIMEOUT 10 LABELS "audio_qa;reference")
endforeach()
