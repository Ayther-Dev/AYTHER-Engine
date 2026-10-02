cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS QA_WORKSPACE_ROOT QA_FIDELITY_PROBE
                           QA_LOOP_OBSERVATION QA_VOICE_END_OBSERVATION
                           QA_LIVE_RESUME)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "Missing required variable: ${_required}")
  endif()
endforeach()

function(require_hash relative expected)
  set(path "${QA_WORKSPACE_ROOT}/${relative}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Frozen oracle artifact is missing: ${relative}")
  endif()
  file(SHA256 "${path}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
            "Frozen oracle artifact changed: ${relative}; expected ${expected}, got ${actual}")
  endif()
endfunction()

function(require_text relative)
  file(READ "${QA_WORKSPACE_ROOT}/${relative}" contents)
  foreach(fragment IN LISTS ARGN)
    string(FIND "${contents}" "${fragment}" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "Frozen case ${relative} lacks: ${fragment}")
    endif()
  endforeach()
endfunction()

set(evidence "evidence")
require_hash("${evidence}/qa-030-frozen-index-r2.json"
             "0774ef15df20a28e0c07b9226adc587fec2d19f46b5f93a990765ecd2152669d")
require_hash("${evidence}/qa-023-oracle-manifest.json"
             "16a83ad1331d06cd6d427c9ff7f6c20ae517db776f74d916ff0280c7b0ebd3ce")
require_hash("${evidence}/qa-026-oracle-manifest.json"
             "4a9cb5e16d86efece34061f8bd4a80370ec0ac961b8eb28a51ce4721e665531a")
require_hash("${evidence}/qa-028-oracle-manifest.json"
             "623db2a17408a075269efe490f5d1af7f4e63cacdc61f40a61a7c345d8102e35")

set(resume "${evidence}/qa-023-capture/resume_voice-c49e23985b44ae4ec40a/observed/staging.toml")
set(loop_run "${evidence}/qa-026-capture/loop-e62656aa825a3febbe83/observed/run.toml")
set(loop_trace "${evidence}/qa-026-capture/loop-e62656aa825a3febbe83/observed/trace.toml")
set(window_run "${evidence}/qa-026-capture/window-b20c0b67dee1b8a95e84/observed/run.toml")
set(window_trace "${evidence}/qa-026-capture/window-b20c0b67dee1b8a95e84/observed/trace.toml")
set(natural_run "${evidence}/qa-028-capture/natural-603c746041b3a783c45b/observed/run.toml")
set(natural_trace "${evidence}/qa-028-capture/natural-603c746041b3a783c45b/observed/trace.toml")

require_hash("${resume}"
             "cba7fb801e24975b9e60edcdef1d6299c3fc40f41896ce7a2228f80e5cc66c4a")
require_hash("${loop_run}"
             "b1927f3243ed716c848a26768d3578bbef8a5f80e449a2ef3e92a383b5165571")
require_hash("${loop_trace}"
             "4e5abcd6e20f7be604a0d1f8c6a16689693bc71669470e97d85ca7c2db401b0d")
require_hash("${window_run}"
             "77a74acc45f41839c9904308707e7f5a05a11739a43bb89705b7db129258035c")
require_hash("${window_trace}"
             "c50c55b26880f8c1d6fec5bc5125a19c00e60a78498170f0409192fc9d627261")
require_hash("${natural_run}"
             "1870ea5419228d891ac3ce2b5736226da5f15a1241ee869d9444854b19bb2178")
require_hash("${natural_trace}"
             "3efb2bd91be3a3d85c098b9e360737feb724ea6c1da6532593e1a6fbf06a572d")

require_text("${resume}" "resume_action = \"restart\""
             "resume_offset_seconds = 1.5"
             "resume_reason = \"resume_at_emulated_offset\"")
require_text("${loop_run}" "loop_begin = 2" "loop_end = 6" "offset = 4")
require_text("${loop_trace}" "kind = \"loop_wrap\"" "output = 2"
             "output = 22" "position_before = 6" "position_after = 2")
require_text("${window_run}" "end_frame = \"2\"" "voices_after_tick = 0")
require_text("${window_trace}" "kind = \"window_end\"" "frame = 3"
             "output = 20" "position_before = 4")
require_text("${natural_run}" "looping = false" "voices_final = 0")
require_text("${natural_trace}" "kind = \"asset_end\"" "output = 12"
             "position_before = 8" "position_after = 8")

foreach(executable IN ITEMS QA_FIDELITY_PROBE QA_LOOP_OBSERVATION
                            QA_VOICE_END_OBSERVATION QA_LIVE_RESUME)
  execute_process(COMMAND "${${executable}}" RESULT_VARIABLE result
                  OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
            "Current loop/end probe ${executable} failed (${result})\n${output}\n${error}")
  endif()
endforeach()

message(STATUS "Frozen loop, window and final positions match current behavior")
