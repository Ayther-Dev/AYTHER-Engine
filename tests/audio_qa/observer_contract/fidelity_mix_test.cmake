cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS QA_WORKSPACE_ROOT QA_FIDELITY_MIX
                           QA_MIX_OBSERVATION QA_LIFETIME_OBSERVATION
                           QA_UNIFIED_MIX QA_FROZEN_PCM)
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

set(evidence "specs/001-AYTHER-bug-audio-qa/evidence")
set(run "${evidence}/qa-029-capture-r2/simultaneous-ebc2d499349e2dcc0a39/observed/run.toml")
set(trace "${evidence}/qa-029-capture-r2/simultaneous-ebc2d499349e2dcc0a39/observed/trace.toml")
set(pcm "${evidence}/qa-029-capture-r2/simultaneous-ebc2d499349e2dcc0a39/observed/output.s16le")
require_hash("${evidence}/qa-030-frozen-index-r2.json"
             "d25f805903e5d2681ed45b3d541a5f337019abb7c25fbfd127de18268eeb3c06")
require_hash("${evidence}/qa-029-oracle-manifest-r2.json"
             "6cf9d3198a7883ffa15415e8acdf5a8b409f2f3017d072f05dd880515771ca1f")
require_hash("${run}"
             "db87ff79eced146b1aa10e9c204588b3e9224c17c45c3231a196db2a2e2a4bfa")
require_hash("${trace}"
             "6fd873532b37d535daaec12cfae8c1a67b512093ea8d42ff3a1746a4409b0dbe")
require_hash("${pcm}"
             "12a7b6b02eaebfb9c14136ee975d025439b38ce55e7b225f614d04b451b10b59")
require_text("${run}" "voices_started = 2" "voices_final = 0"
             "gain = 0.5" "gain = 0.25" "fade_frames = 4")
require_text("${trace}" "kind = \"cut_frame\""
             "kind = \"authored_fade_begin\"" "kind = \"fade_complete\""
             "gain = 0.375" "gain = 0.125")

execute_process(COMMAND "${QA_FIDELITY_MIX}" "${QA_FROZEN_PCM}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Current simultaneous mix differs (${result})\n${output}\n${error}")
endif()
foreach(executable IN ITEMS QA_MIX_OBSERVATION QA_LIFETIME_OBSERVATION
                            QA_UNIFIED_MIX)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env SDL_AUDIO_DRIVER=dummy
                          "${${executable}}"
                  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Current mix probe ${executable} failed (${result})\n${output}\n${error}")
  endif()
endforeach()

message(STATUS "Frozen simultaneous participants, gains, effects and PCM match")
