# Included by the conversion experiment: reuse its pinned SDK and observed SDL.
set(reference "${QA_ENGINE_REFERENCE_SOURCE}")
set(QA_DECODER_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/windows-native/vcpkg_installed/x64-windows/include"
    CACHE PATH "Existing decoder declarations used by the published Engine")
file(SHA256 "${QA_DECODER_INCLUDE}/dr_flac.h" decoder_sha)
if(NOT decoder_sha STREQUAL "a35468f278f17cf9538ce189debf76a5854a77e139980f7b5185d67566a0c837")
    message(FATAL_ERROR "Decoder declarations changed")
endif()
foreach(entry IN ITEMS
    "src/audio_player.cpp|cc071e307e76c670b62c79e5fcfcc4e9258f80abdc36fa0f88abc1c28f84ba97"
    "include/ayther/audio_player.h|0023b27fb727d970072e72bd8cb29803447f7615b2cbee377c90d173df0df759")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 expected)
    file(SHA256 "${reference}/${file}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Staging reference changed: ${file}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${reference}/${file}")
endforeach()
file(READ "${reference}/src/audio_player.cpp" source)
file(READ "${reference}/include/ayther/audio_player.h" header)
string(REPLACE "\r\n" "\n" source "${source}")
string(REPLACE "\r\n" "\n" header "${header}")
string(FIND "${source}" "static constexpr SDL_AudioSpec kEmuSpec" begin)
string(FIND "${source}" "void AudioPlayer::play_substitutions" end)
if(begin LESS 0 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Staging section anchors missing")
endif()
math(EXPR length "${end} - ${begin}")
string(SUBSTRING "${source}" ${begin} ${length} methods)
foreach(anchor IN ITEMS "void AudioPlayer::stop_all_sfx() {"
    "void AudioPlayer::stop_all_events() {" "void AudioPlayer::stop_oneshot() {"
    "bool AudioPlayer::play_event_hd(" "HdMixPcm AudioPlayer::get_mix_pcm("
    "const AudioPlayer::WavEntry*\nAudioPlayer::get_wav("
    "bool AudioPlayer::decode_audio_bytes(" "std::string asset_ext(")
    string(FIND "${source}" "${anchor}" begin)
    if(begin LESS 0)
        message(FATAL_ERROR "Method anchor missing: ${anchor}")
    endif()
    string(SUBSTRING "${source}" ${begin} -1 tail)
    string(FIND "${tail}" "\n}\n" end)
    math(EXPR length "${end} + 3")
    string(SUBSTRING "${tail}" 0 ${length} method_source)
    # The extension helper must precede get_wav; definitions otherwise retain source order.
    if(anchor STREQUAL "std::string asset_ext(")
        string(PREPEND methods "namespace {\n${method_source}\n}\n")
    else()
        string(APPEND methods "\n${method_source}")
    endif()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/reference_methods.inc" "${methods}")
function(observe variable anchor replacement)
    string(FIND "${${variable}}" "${anchor}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Staging observation anchor missing: ${anchor}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" changed "${${variable}}")
    set(${variable} "${changed}" PARENT_SCOPE)
endfunction()
set(snapshot "qa_stage::Coordinates{frame_mark_, timeline_samples_}, pending_pcm_, pending_batches_")
observe(header "void mark_frame_boundary() { frame_mark_ = pending_pcm_.size() / 2; }"
    "void mark_frame_boundary() { frame_mark_ = pending_pcm_.size() / 2; qa_stage::record(\"frame\", ${snapshot}); }")
observe(methods "    pending_batches_.push_back({ hash, frame_offset, frames });"
    "    pending_batches_.push_back({ hash, frame_offset, frames });\n    qa_stage::record(\"batch\", ${snapshot});")
observe(methods "            PendingBatch{ kRouterHash, 0, pending_pcm_.size() / 2 });"
    "            PendingBatch{ kRouterHash, 0, pending_pcm_.size() / 2 });\n        qa_stage::record(\"router_add\", ${snapshot});")
observe(methods "    frame_mark_ = 0;\n}\n\nvoid AudioPlayer::clear_synth()"
    "    frame_mark_ = 0;\n    qa_stage::record(\"router_replace\", ${snapshot});\n}\n\nvoid AudioPlayer::clear_synth()")
observe(methods "void AudioPlayer::flush_emulator(bool suppress_original) {"
    "void AudioPlayer::flush_emulator(bool suppress_original) {\n    qa_stage::record(\"flush_begin\", ${snapshot});")
observe(methods "            const size_t frames = pending_pcm_.size() / 2;"
    "            qa_stage::record(\"pre_mix\", ${snapshot});\n            const size_t frames = pending_pcm_.size() / 2;")
observe(methods "                                   timeline_samples_);"
    "                                   timeline_samples_);\n                qa_stage::record(\"post_mix\", ${snapshot});")
observe(methods "void AudioPlayer::discard_emulator() {"
    "void AudioPlayer::discard_emulator() {\n    qa_stage::record(\"discard\", ${snapshot});")
observe(methods "    frame_mark_ = 0;            // : el próximo frame arranca el bloque"
    "    frame_mark_ = 0;            // : el próximo frame arranca el bloque\n    qa_stage::record(\"discarded\", ${snapshot});")
# A separate test class avoids overriding or mixing the published SDK's symbols.
string(REPLACE "AudioPlayer" "QaStagePlayer" methods "${methods}")
string(REPLACE "AudioPlayer" "QaStagePlayer" header "${header}")
file(SHA256 "${reference}/include/ayther/audio_hd_mixer.h" mixer_sha)
if(NOT mixer_sha STREQUAL "f47f24ca73632799da944f2ec5b8425221c2deda475291f1c9263fd02448dae8")
    message(FATAL_ERROR "Staging mixer reference changed")
endif()
file(READ "${reference}/include/ayther/audio_hd_mixer.h" mixer)
string(REPLACE "\r\n" "\n" mixer "${mixer}")
observe(mixer "                const size_t o = f * 2;"
    "                const size_t o = f * 2;\n                const int16_t qa_left = out[o], qa_right = out[o + 1];")
observe(mixer "                ++v.pos;"
    "                ++v.pos;\n                qa_stage::record_voice({v.key, block_start + f, s / 2, v.pos, g, qa_left, qa_right, out[o], out[o + 1]});")
string(REPLACE "HdMixer" "QaStageMixer" mixer "${mixer}")
string(REPLACE "HdMixer" "QaStageMixer" header "${header}")
string(REPLACE "\"audio_hd_mixer.h\"" "\"stage_mixer.h\"" header "${header}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stage_mixer.h" "// AYTHER QA observation copy.\n${mixer}")
file(SHA256 "${reference}/include/ayther/audio_live_resume.h" resume_sha)
if(NOT resume_sha STREQUAL "60c64ed5f4cecfb1119206c942f3590f9e3dd1aaabb2312f04b8126667d3ad26")
    message(FATAL_ERROR "Resume reference changed")
endif()
file(READ "${reference}/include/ayther/audio_live_resume.h" resume)
string(REPLACE "\r\n" "\n" resume "${resume}")
observe(resume "    return {LiveResumeAction::Restart, offset};"
    "    qa_stage::record_resume_reason(\"resume_at_emulated_offset\");\n    return {LiveResumeAction::Restart, offset};")
# Avoid replacing inline symbols that may also be present in the published SDK.
string(REPLACE "live_resume_decide" "qa_live_resume_decide" resume "${resume}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stage_resume.h" "// AYTHER QA observation copy.\n${resume}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stage_player.h" "// AYTHER QA observation copy.\n${header}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/observed_methods.inc" "// AYTHER QA observation copy.\n${methods}")
foreach(kind IN ITEMS reference observed)
    add_executable(audio_qa_staging_${kind} staging_probe.cpp)
    target_compile_features(audio_qa_staging_${kind} PRIVATE cxx_std_20)
    target_compile_options(audio_qa_staging_${kind} PRIVATE /W4 /WX /permissive- /EHsc)
    target_include_directories(audio_qa_staging_${kind} SYSTEM PRIVATE "${reference}/include"
        "${reference}/include/ayther" "${CMAKE_CURRENT_BINARY_DIR}" "${QA_DECODER_INCLUDE}")
    target_include_directories(audio_qa_staging_${kind} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../conversion_probe")
    target_link_libraries(audio_qa_staging_${kind} PRIVATE Ayther::engine SDL3::SDL3)
    set_target_properties(audio_qa_staging_${kind} PROPERTIES CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${kind}")
    if(kind STREQUAL "observed")
        target_compile_definitions(audio_qa_staging_${kind} PRIVATE QA_STAGE_OBSERVER=1)
    else()
        target_compile_definitions(audio_qa_staging_${kind} PRIVATE QA_STAGE_OBSERVER=0)
    endif()
    add_custom_command(TARGET audio_qa_staging_${kind} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QA_RUNTIME_BIN}" "$<TARGET_FILE_DIR:audio_qa_staging_${kind}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3-shared>"
            "$<TARGET_FILE_DIR:audio_qa_staging_${kind}>/SDL3.dll" VERBATIM)
endforeach()
foreach(mode IN ITEMS batches mute add replace discard suppress lost voice voice_suppress voice_lost resume_voice resume_lost)
    add_test(NAME audio_qa_staging_${mode}
        COMMAND ${CMAKE_COMMAND} "-DREFERENCE=$<TARGET_FILE:audio_qa_staging_reference>"
            "-DOBSERVED=$<TARGET_FILE:audio_qa_staging_observed>" "-DMODE=${mode}"
            "-DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>"
            "-DPACK=${CMAKE_CURRENT_SOURCE_DIR}/../conversion_probe/data/common-mix.ay"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake")
    set_tests_properties(audio_qa_staging_${mode} PROPERTIES TIMEOUT 15 LABELS "audio_qa;integration"
        ENVIRONMENT "SDL_AUDIO_DRIVER=dummy;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=512;SDL_AUDIO_CHANNELS=2")
endforeach()
