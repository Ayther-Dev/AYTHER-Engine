cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS QA_WORKSPACE_ROOT QA_MATCH_SESSION
                           QA_SEQUENCE_SESSION)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "Missing required variable: ${_required}")
  endif()
endforeach()

set(index
    "${QA_WORKSPACE_ROOT}/specs/001-AYTHER-bug-audio-qa/evidence/qa-030-frozen-index-r2.json")
if(NOT EXISTS "${index}")
  message(FATAL_ERROR "The independent frozen oracle index is missing")
endif()
file(SHA256 "${index}" index_hash)
if(NOT index_hash STREQUAL
   "d25f805903e5d2681ed45b3d541a5f337019abb7c25fbfd127de18268eeb3c06")
  message(FATAL_ERROR "The independent frozen oracle index changed")
endif()

foreach(executable IN ITEMS QA_MATCH_SESSION QA_SEQUENCE_SESSION)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env SDL_AUDIO_DRIVER=dummy
                          "${${executable}}"
                  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
            "Observer neutrality failed for ${executable} (${result})\n${output}\n${error}")
  endif()
endforeach()

message(STATUS
        "Enabled, disabled and dropping observers preserve inputs and decisions; frozen comparisons remain independent")
