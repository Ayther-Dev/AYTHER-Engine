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

set(evidence "specs/001-AYTHER-bug-audio-qa/evidence")
require_hash("${evidence}/qa-030-frozen-index-r2.json"
             "d25f805903e5d2681ed45b3d541a5f337019abb7c25fbfd127de18268eeb3c06")
require_hash("${evidence}/qa-023-oracle-manifest.json"
             "025efd3557f572dd30ee27054d259cd1a74760758cf4e82f575ee44f09f183b3")
require_hash("${evidence}/qa-026-oracle-manifest.json"
             "bd2132a3b6e26c1afac92c72d46ecf6cf8c37d981e2e70b587baa04f093a8a32")
require_hash("${evidence}/qa-028-oracle-manifest.json"
             "655ed825919c5c086c772b593c6765763f0336036da1237d725a01c71570a9c7")

set(resume "${evidence}/qa-023-capture/resume_voice-c49e23985b44ae4ec40a/observed/staging.toml")
set(loop_run "${evidence}/qa-026-capture/loop-e62656aa825a3febbe83/observed/run.toml")
set(loop_trace "${evidence}/qa-026-capture/loop-e62656aa825a3febbe83/observed/trace.toml")
set(window_run "${evidence}/qa-026-capture/window-b20c0b67dee1b8a95e84/observed/run.toml")
set(window_trace "${evidence}/qa-026-capture/window-b20c0b67dee1b8a95e84/observed/trace.toml")
set(natural_run "${evidence}/qa-028-capture/natural-603c746041b3a783c45b/observed/run.toml")
set(natural_trace "${evidence}/qa-028-capture/natural-603c746041b3a783c45b/observed/trace.toml")

require_hash("${resume}"
             "b8592cbe7cd1fa59efd0a9e25cb2fdccbd61ba6310c0484385ff1d8ba9b27d7a")
require_hash("${loop_run}"
             "771d5a3a979c8a040b60f297854ad8fc548482bc710c7a379c3b073a206a5ccd")
require_hash("${loop_trace}"
             "8827ed088796a2fd7f828a7595068483971857f7f5be194bf30adab5f7db5657")
require_hash("${window_run}"
             "5e16450640e9e3d85b53b90d4f757f8f667cca805dff08d2f202c1105476f3df")
require_hash("${window_trace}"
             "4e32917cf902a40fdada1ef9495123b3398c4accf89d76ba70b942dcfbd19b72")
require_hash("${natural_run}"
             "808ce1a2465d536947bdff3020e5ae5fdda2bc8dec6f9af4c9afe122e1329fab")
require_hash("${natural_trace}"
             "493a9ae34c4f03e751e9e9f1a124d37e7d109c6f71970190d4403268fddea79c")

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
