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

set(evidence "specs/001-AYTHER-bug-audio-qa/evidence")
require_hash("${evidence}/qa-030-frozen-index-r2.json"
             "d25f805903e5d2681ed45b3d541a5f337019abb7c25fbfd127de18268eeb3c06")
require_hash("${evidence}/qa-024-oracle-manifest.json"
             "33f794e94b1040ebbeffe19ee1e676272c6c0744b8b82c505aa75d221ebde7b1")
require_hash("${evidence}/qa-025-oracle-manifest.json"
             "179e84debe5e1842b72224ddee6b1bab12691b1f79a1e3f8e462ce4f428495eb")

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
             "a16f2849f31ecde7cde61cb2784a5a9e8bb3f44c0ec5a5201f9638287e612dbf")
require_hash("${match_facts}"
             "133edbf71f26c536a648a3ea79510e21544a53613b194436b21e722af5adbf13")
require_hash("${miss_decision}"
             "1911e4e01e4539855453dafcbc89527006e1f6179dd252558ecddc85b03bf6e7")
require_hash("${miss_facts}"
             "0d3c6ffe1db1aeb785dbd119b82f55310cafa4853aafb8ebff8671a8f405ea3a")
require_hash("${exact_decision}"
             "187c5298894fd8e9df8b58edf4e10c6e828bcd6dac1770f093a6837113b6c0e4")
require_hash("${exact_facts}"
             "49fb60bbee2da620a2c69fa3fed75cfab8a3747612170b4c49c599a79ecf247b")
require_hash("${keep_run}"
             "0669864df3d778789abd7199e44e2b05ce84d6e76fe4f681015566d92f33a5fb")
require_hash("${keep_trace}"
             "3199a82ff426ad54d42d40d2786124d1e437d6870d9da2b69c295644e0a3f262")
require_hash("${repeat_run}"
             "112e8e7e2091040e359402a41ebad520b8deeff00474332cdcccbf753a3409ae")
require_hash("${repeat_trace}"
             "f1da825c5cb2caa4e08f7cea96affdbebbe1fb3526d41ff638542925b4495049")

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
