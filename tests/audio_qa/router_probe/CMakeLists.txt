cmake_minimum_required(VERSION 3.25)
project(AytherRouterProbe LANGUAGES CXX)
enable_testing()
set(reference "${QA_ENGINE_REFERENCE_SOURCE}")
foreach(entry IN ITEMS
    "src/voice_router.cpp|e1a5166f911bc74f0f86c42b4c7441dc6d4e066257fa2f092b6bdb2579ff67fc"
    "include/ayther/voice_router.h|f868b195147b7fba6a2841c59ca3782adc6e4a8fa6f2f6b25c76621aebaf03fe"
    "src/ayther_session_impl.inl|f63f1412b8274433beb2ee536980f4a4448f1eab00b55e672ed4ac5020dfb8c6")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 expected)
    file(SHA256 "${reference}/${file}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Reference identity differs: ${file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${reference}/${file}")
endforeach()
function(section input first last output)
    string(FIND "${input}" "${first}" begin)
    string(FIND "${input}" "${last}" end)
    if(begin LESS 0 OR end LESS_EQUAL begin)
        message(FATAL_ERROR "Reference section boundaries missing")
    endif()
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${input}" ${begin} ${length} part)
    set(${output} "${part}" PARENT_SCOPE)
endfunction()
file(READ "${reference}/src/voice_router.cpp" source)
string(REPLACE "\r\n" "\n" source "${source}")
section("${source}" "namespace {\nconstexpr int    kRsTaps" "// ---------------------------------------------------------------------------\n// CopySource" functions)
file(READ "${reference}/src/ayther_session_impl.inl" session)
string(REPLACE "\r\n" "\n" session "${session}")
section("${session}" "        if (voice_rs.available() > n * 3)" "        // La voz del SoundFont entra acá" boundary)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/boundary.inc" "${boundary}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/reference.inc" "namespace ayther {\n${functions}\n}\n")
function(observe anchor replacement)
    string(FIND "${functions}" "${anchor}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Observation anchor missing: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" functions "${functions}")
    set(functions "${functions}" PARENT_SCOPE)
endfunction()
observe("void StreamResampler::reset() { buf_.clear(); pos_ = 0.0; }"
    "void StreamResampler::reset() { const double qa_before = pos_; const size_t qa_size = buf_.size()/2; buf_.clear(); pos_ = 0.0; qa_record({Kind::reset, qa_before, pos_, step_, qa_size, buf_.size()/2}); }")
observe("    step_ = (dst_hz > 0.0) ? (src_hz / dst_hz) : 1.0;"
    "    step_ = (dst_hz > 0.0) ? (src_hz / dst_hz) : 1.0;\n    qa_record({Kind::rate, pos_, pos_, step_, buf_.size()/2, buf_.size()/2});")
observe("    buf_.insert(buf_.end(), in, in + frames * 2);"
    "    const size_t qa_size = buf_.size()/2;\n    buf_.insert(buf_.end(), in, in + frames * 2);\n    qa_record({Kind::push, pos_, pos_, step_, qa_size, buf_.size()/2, frames});")
observe("        const long c = long(std::floor(pos_));"
    "        const double qa_before = pos_;\n        long qa_first = -1, qa_last = -1;\n        const long c = long(std::floor(pos_));")
observe("            const double x  = pos_ - double(idx);"
    "            if (qa_first < 0) qa_first = idx;\n            qa_last = idx;\n            const double x  = pos_ - double(idx);")
observe("        pos_ += step_;"
    "        pos_ += step_;\n        qa_record({Kind::sample, qa_before, pos_, step_, n_in, n_in, i, qa_first, qa_last, out[i*2], out[i*2+1]});")
observe("    if (keep_from > 0) {"
    "    if (keep_from > 0) {\n        const double qa_before = pos_; const size_t qa_size = buf_.size()/2;")
observe("        pos_ -= double(keep_from);"
    "        pos_ -= double(keep_from);\n        qa_record({Kind::trim, qa_before, pos_, step_, qa_size, buf_.size()/2, size_t(keep_from)});")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/observed.inc"
    "// AYTHER QA disposable observation copy, not production source.\nnamespace ayther {\n${functions}\n}\n")
foreach(kind IN ITEMS reference observed)
    add_executable(audio_qa_router_${kind} router_probe.cpp)
    target_compile_features(audio_qa_router_${kind} PRIVATE cxx_std_20)
    target_compile_options(audio_qa_router_${kind} PRIVATE /W4 /WX /permissive- /EHsc)
    set_target_properties(audio_qa_router_${kind} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(audio_qa_router_${kind} SYSTEM PRIVATE "${reference}/include" "${reference}/include/ayther" "${CMAKE_CURRENT_BINARY_DIR}")
    if(kind STREQUAL "observed")
        target_compile_definitions(audio_qa_router_${kind} PRIVATE QA_EXPECT_OBSERVER=1)
    else()
        target_compile_definitions(audio_qa_router_${kind} PRIVATE QA_EXPECT_OBSERVER=0)
    endif()
endforeach()
foreach(mode IN ITEMS complete lost phase)
    add_test(NAME audio_qa_router_${mode}
        COMMAND ${CMAKE_COMMAND} "-DREFERENCE=$<TARGET_FILE:audio_qa_router_reference>"
            "-DOBSERVED=$<TARGET_FILE:audio_qa_router_observed>" "-DMODE=${mode}"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake")
    set_tests_properties(audio_qa_router_${mode} PROPERTIES TIMEOUT 10 LABELS "audio_qa;integration")
endforeach()
