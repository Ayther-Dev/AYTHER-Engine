cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS QA_WORKSPACE_ROOT QA_MATCH_OBSERVATION
                           QA_MATCH_CANDIDATES QA_PLAYBACK_OBSERVATION
                           QA_PLAYBACK_TRANSITION QA_MATCH_SESSION
                           QA_WORKING_DIRECTORY)
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
require_hash("${evidence}/qa-024-oracle-manifest.json"
             "6f3db0035e3e8cca1a035b50904ca6aef9443fc2f5a3185d2dbac1f097696318")
require_hash("${evidence}/qa-025-oracle-manifest.json"
             "771c4bc329dd15a1e7e6205494c4868ca42b72fa152a8d1aa0081c6ec7ccc041")

set(match_decision
    "${evidence}/qa-024-capture/match-fc60ea56b66eed96ce88/observed/decision.toml")
set(match_facts
    "${evidence}/qa-024-capture/match-fc60ea56b66eed96ce88/observed/facts.toml")
set(miss_decision
    "${evidence}/qa-024-capture/miss-0c1cbaa08f58c23e7b29/observed/decision.toml")
set(miss_facts
    "${evidence}/qa-024-capture/miss-0c1cbaa08f58c23e7b29/observed/facts.toml")
set(exact_decision
    "${evidence}/qa-024-capture/exact-657cc205d20f643008a1/observed/decision.toml")
set(exact_facts
    "${evidence}/qa-024-capture/exact-657cc205d20f643008a1/observed/facts.toml")
set(keep_run
    "${evidence}/qa-025-capture/keep-95d11e041ecbfb3b8073/observed/run.toml")
set(keep_trace
    "${evidence}/qa-025-capture/keep-95d11e041ecbfb3b8073/observed/trace.toml")
set(repeat_run
    "${evidence}/qa-025-capture/repeat-7da8a669178f1f8fa065/observed/run.toml")
set(repeat_trace
    "${evidence}/qa-025-capture/repeat-7da8a669178f1f8fa065/observed/trace.toml")

require_hash("${match_decision}"
             "0e1d2be938c3ecfba1a48f651ca44a3059d1885ed2995cd12460e1d9f913b15d")
require_hash("${match_facts}"
             "93b352d88ee2e64e5d280e29a4beed6b492269835098151c3854bf1663703ac1")
require_hash("${miss_decision}"
             "d2ec02c51d5bcbecd1aa9965186d0dacbe8203d2323a55c6d871dca19b0363d8")
require_hash("${miss_facts}"
             "2025325489c3704a45f3c2be30921288081bb91d2800ca9289335b4a279e6737")
require_hash("${exact_decision}"
             "37a445ff0956a8717193d3913492d59b3856229fb12a412d65218d11c2bc168f")
require_hash("${exact_facts}"
             "9ab21d963e4d50e57f619057a8d77f347f492b9e972699adc6c68c519d7bb0a8")
require_hash("${keep_run}"
             "8ebc22c6378496629f80a0c3a59f1bb835b817ae53481c9bde47ea7bfe303d75")
require_hash("${keep_trace}"
             "8f6c39c042ca864cb6204f6ac5bcf08a023e1c59fe4481ea6ecf0437e98e465a")
require_hash("${repeat_run}"
             "5f205045dd4f5b1255a5f88d266aa6cea0cb004b4c495d9157199f7e9ed3efa5")
require_hash("${repeat_trace}"
             "3c5565a40edc41549a670af0508cbae1549c9ed05189650840ed2c07fd91c6da")

require_text("${match_decision}" "matched = true" "selected = \"15\"")
require_text("${match_facts}" "kind = \"pitch_rejected\""
             "kind = \"winner_updated\"" "kind = \"selected\"")
require_text("${miss_decision}" "matched = false"
             "selected = \"18446744073709551615\"")
require_text("${miss_facts}" "kind = \"pitch_rejected\""
             "kind = \"no_match\"")
require_text("${exact_decision}" "matched = true" "selected = \"99\"")
require_text("${exact_facts}" "kind = \"exact_selected\"")
require_text("${keep_run}" "voices_started = 1" "voices_active = 1")
require_text("${keep_trace}" "branch = \"rising_edge\""
             "branch = \"gate_not_entered\"")
require_text("${repeat_run}" "voices_started = 2" "voices_active = 2")
require_text("${repeat_trace}" "branch = \"rising_edge\""
             "branch = \"gate_not_entered\"")

file(MAKE_DIRECTORY "${QA_WORKING_DIRECTORY}")
foreach(executable IN ITEMS QA_MATCH_CANDIDATES QA_MATCH_OBSERVATION
                            QA_PLAYBACK_OBSERVATION QA_PLAYBACK_TRANSITION
                            QA_MATCH_SESSION)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env SDL_AUDIO_DRIVER=dummy
            "${${executable}}"
    WORKING_DIRECTORY "${QA_WORKING_DIRECTORY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
            "Current fidelity probe ${executable} failed (${result})\n${output}\n${error}")
  endif()
endforeach()

message(STATUS
        "Frozen selection/trigger decisions match current decisions, reasons and effects")
