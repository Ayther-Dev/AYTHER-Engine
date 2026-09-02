# ---------------------------------------------------------------------------
# check_public_headers.cmake -- verify that installed headers include only
# headers that are part of the installed public surface.
#
# The public surface is an explicit allowlist. An in-tree build can accidentally
# hide a missing installed dependency because it sees every project header.
# Running this check through CTest catches that packaging error before install.
#
# Uso:
#   cmake -DAYTHER_INCLUDE_DIR=<dir> -DAYTHER_PUBLIC_HEADERS=<lista;;> \
#         -P tools/check_public_headers.cmake
# ---------------------------------------------------------------------------
# Script mode starts with default policy values. Requiring CMake 3.21 ensures
# `IN_LIST` has the behavior expected by this check on local and CI machines.
cmake_minimum_required(VERSION 3.21)
if(NOT AYTHER_INCLUDE_DIR OR NOT AYTHER_PUBLIC_HEADERS)
    message(FATAL_ERROR
        "Missing -DAYTHER_INCLUDE_DIR or -DAYTHER_PUBLIC_HEADERS")
endif()

set(_public_headers ${AYTHER_PUBLIC_HEADERS})
set(_pending ${_public_headers})
set(_visited "")
set(_missing "")

while(_pending)
    list(POP_FRONT _pending _header)
    if(_header IN_LIST _visited)
        continue()
    endif()
    list(APPEND _visited "${_header}")

    set(_path "${AYTHER_INCLUDE_DIR}/${_header}")
    if(NOT EXISTS "${_path}")
        list(APPEND _missing "${_header} (declared public but does not exist)")
        continue()
    endif()

    file(STRINGS "${_path}" _lines
        REGEX "^[ \t]*#[ \t]*include[ \t]*(\"|<ayther/)")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES
           "(src|libretro_host|vulkan_backend|private|internal|detail)/")
            list(APPEND _missing
                "${_header} includes a forbidden private path: ${_line}")
        endif()
        if(_line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(_dependency "${CMAKE_MATCH_1}")
        elseif(_line MATCHES
               "^[ \t]*#[ \t]*include[ \t]*<ayther/([^>]+)>")
            set(_dependency "${CMAKE_MATCH_1}")
        else()
            continue()
        endif()
        if(NOT _dependency IN_LIST _public_headers)
            list(APPEND _missing
                "${_header} includes \"${_dependency}\", which is not installed")
        endif()
        list(APPEND _pending "${_dependency}")
    endforeach()
endwhile()

if(_missing)
    message("")
    foreach(_failure IN LISTS _missing)
        message("  [FAIL] ${_failure}")
    endforeach()
    message(FATAL_ERROR
        "The installed public-header surface is not closed. Either add the "
        "dependency to the package contract or remove it from the public header.")
endif()

list(LENGTH _visited _count)
message("  [ OK ] public surface is closed: ${_count} headers")

# An empty allowlist or a wrong include directory must not pass vacuously.
if(_count LESS 2)
    message(FATAL_ERROR
        "Non-empty check failed: visited ${_count} headers; verify the public "
        "allowlist and include directory")
endif()
