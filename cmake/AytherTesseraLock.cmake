# The Tessera lock (dependencies/tessera.lock.json) and the checks against it
# (TP-016.1, TP-016.2).
#
# The Engine builds against one exact Tessera: the lock names its source commit,
# its API version and, once Tessera publishes a tag with that API, the tag and
# the SHA-256 of the package. Two functions check it:
#
#   ayther_verify_tessera_lock(<out> LOCK <file> [PREFIX <dir>])
#       <out> receives a list of problems, empty when the lock is well formed
#       and, with PREFIX (an installed Tessera package), when the package is the
#       one the lock names.
#
#   ayther_require_tessera_api(<major> <minor> <prefix>)
#       Fails when the installed package provides an older API.
#
# Run as a script it is the lock test:
#   cmake -DAYTHER_TESSERA_LOCK=<file> [-DAYTHER_TESSERA_PREFIX=<dir>]
#         -P tools/check_tessera_lock.cmake

# The API the Engine needs from Tessera. Raising it is a deliberate change of
# this constant and of the lock.
set(AYTHER_TESSERA_MIN_API_MAJOR 1)
set(AYTHER_TESSERA_MIN_API_MINOR 1)

# The API version a Tessera package declares in its TesseraConfig.cmake. The
# package-metadata.json carries an `abi` object too, but the configuration file
# is what find_package() itself reads.
function(ayther_tessera_package_api out_major out_minor prefix)
    set(_config "${prefix}/lib/cmake/Tessera/TesseraConfig.cmake")
    set(_major "")
    set(_minor "")
    if(EXISTS "${_config}")
        file(STRINGS "${_config}" _lines REGEX "^set\\(Tessera_ABI_(MAJOR|MINOR) [0-9]+\\)")
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "Tessera_ABI_MAJOR ([0-9]+)")
                set(_major "${CMAKE_MATCH_1}")
            elseif(_line MATCHES "Tessera_ABI_MINOR ([0-9]+)")
                set(_minor "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()
    set("${out_major}" "${_major}" PARENT_SCOPE)
    set("${out_minor}" "${_minor}" PARENT_SCOPE)
endfunction()

function(ayther_require_tessera_api major minor prefix)
    ayther_tessera_package_api(_have_major _have_minor "${prefix}")
    if(_have_major STREQUAL "" OR _have_minor STREQUAL "")
        message(FATAL_ERROR
            "TESSERA_API_UNKNOWN: ${prefix} does not declare Tessera_ABI_MAJOR "
            "and Tessera_ABI_MINOR in lib/cmake/Tessera/TesseraConfig.cmake.")
    endif()
    if(NOT _have_major EQUAL ${major} OR _have_minor LESS ${minor})
        message(FATAL_ERROR
            "TESSERA_API_TOO_OLD: the Engine requires the Tessera API "
            "${major}.${minor} or a later 1.x, but the package at ${prefix} "
            "provides ${_have_major}.${_have_minor}. Build and install the "
            "Tessera commit named in dependencies/tessera.lock.json and point "
            "Tessera_DIR at it. The 1.0 package is not accepted by the Engine.")
    endif()
endfunction()

# JSON helpers: `string(JSON ... GET)` fails on a missing member, and a null is
# a member whose TYPE is NULL.
function(_ayther_lock_member out json)
    string(JSON _type ERROR_VARIABLE _error TYPE "${json}" ${ARGN})
    if(NOT _error STREQUAL "NOTFOUND")
        set("${out}" "<missing>" PARENT_SCOPE)
    elseif(_type STREQUAL "NULL")
        set("${out}" "<null>" PARENT_SCOPE)
    else()
        string(JSON _value GET "${json}" ${ARGN})
        set("${out}" "${_value}" PARENT_SCOPE)
    endif()
endfunction()

# CMake regular expressions have no {n} quantifier: check the digits and the
# length separately.
function(_ayther_is_hex out value length)
    string(LENGTH "${value}" _length)
    if(_length EQUAL ${length} AND value MATCHES "^[0-9a-f]+$")
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(ayther_verify_tessera_lock out_problems)
    cmake_parse_arguments(ARG "" "LOCK;PREFIX" "" ${ARGN})
    set(_problems "")

    if(NOT EXISTS "${ARG_LOCK}")
        set("${out_problems}" "TESSERA_LOCK_MISSING: ${ARG_LOCK}" PARENT_SCOPE)
        return()
    endif()
    file(READ "${ARG_LOCK}" _json)
    string(JSON _probe ERROR_VARIABLE _json_error TYPE "${_json}" schemaVersion)
    if(NOT _json_error STREQUAL "NOTFOUND")
        set("${out_problems}"
            "TESSERA_LOCK_INVALID: ${ARG_LOCK} is not a lock file (${_json_error})"
            PARENT_SCOPE)
        return()
    endif()

    _ayther_lock_member(_schema "${_json}" schemaVersion)
    _ayther_lock_member(_commit "${_json}" sourceCommit)
    _ayther_lock_member(_api_major "${_json}" api major)
    _ayther_lock_member(_api_minor "${_json}" api minor)
    _ayther_lock_member(_package_version "${_json}" package version)
    _ayther_lock_member(_tag "${_json}" release tag)
    _ayther_lock_member(_status "${_json}" release status)
    _ayther_lock_member(_file "${_json}" artifact fileName)
    _ayther_lock_member(_sha "${_json}" artifact sha256)

    if(NOT _schema STREQUAL "1")
        list(APPEND _problems "TESSERA_LOCK_INVALID: schemaVersion must be 1, got ${_schema}")
    endif()
    _ayther_is_hex(_commit_ok "${_commit}" 40)
    if(NOT _commit_ok)
        list(APPEND _problems
            "TESSERA_LOCK_INVALID: sourceCommit must be a full lowercase SHA-1, got ${_commit}")
    endif()
    if(NOT _api_major MATCHES "^[0-9]+$" OR NOT _api_minor MATCHES "^[0-9]+$")
        list(APPEND _problems "TESSERA_LOCK_INVALID: api.major and api.minor must be numbers")
    elseif(NOT _api_major EQUAL ${AYTHER_TESSERA_MIN_API_MAJOR} OR
           _api_minor LESS ${AYTHER_TESSERA_MIN_API_MINOR})
        list(APPEND _problems
            "TESSERA_LOCK_API: the lock names the API ${_api_major}.${_api_minor}, the Engine requires ${AYTHER_TESSERA_MIN_API_MAJOR}.${AYTHER_TESSERA_MIN_API_MINOR}")
    endif()
    if(NOT _package_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        list(APPEND _problems "TESSERA_LOCK_INVALID: package.version must be MAJOR.MINOR.PATCH")
    endif()

    # A released lock pins a tag and the package SHA-256; an unreleased one has
    # neither and says so. Anything in between is a half-edited lock.
    if(_tag STREQUAL "<null>")
        if(NOT _status STREQUAL "unreleased")
            list(APPEND _problems
                "TESSERA_LOCK_INVALID: release.tag is null, so release.status must be \"unreleased\"")
        endif()
        if(NOT _sha STREQUAL "<null>" OR NOT _file STREQUAL "<null>")
            list(APPEND _problems
                "TESSERA_LOCK_INVALID: an unreleased lock has no artifact file name or SHA-256")
        endif()
    else()
        if(NOT _tag MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.]+)?$")
            list(APPEND _problems "TESSERA_LOCK_INVALID: release.tag is not a version tag: ${_tag}")
        endif()
        if(NOT _status STREQUAL "released")
            list(APPEND _problems
                "TESSERA_LOCK_INVALID: release.tag is set, so release.status must be \"released\"")
        endif()
        _ayther_is_hex(_sha_ok "${_sha}" 64)
        if(NOT _sha_ok)
            list(APPEND _problems
                "TESSERA_LOCK_INVALID: a released lock needs artifact.sha256 (64 hex digits), got ${_sha}")
        endif()
        if(_file STREQUAL "<null>" OR _file STREQUAL "<missing>")
            list(APPEND _problems "TESSERA_LOCK_INVALID: a released lock needs artifact.fileName")
        endif()
        # The tag names the package version: v0.2.0-rc.1 is package 0.2.0.
        if(_tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)")
            if(NOT CMAKE_MATCH_1 STREQUAL _package_version)
                list(APPEND _problems
                    "TESSERA_LOCK_INVALID: release.tag ${_tag} does not match package.version ${_package_version}")
            endif()
        endif()
    endif()

    # The installed package must be the one the lock names.
    if(ARG_PREFIX)
        set(_metadata "${ARG_PREFIX}/share/tessera/package-metadata.json")
        if(NOT EXISTS "${_metadata}")
            list(APPEND _problems
                "TESSERA_LOCK_MISMATCH: no package metadata at ${_metadata}")
        else()
            file(READ "${_metadata}" _meta)
            _ayther_lock_member(_meta_version "${_meta}" version)
            _ayther_lock_member(_meta_revision "${_meta}" source revision)
            if(NOT _meta_revision STREQUAL _commit)
                list(APPEND _problems
                    "TESSERA_LOCK_MISMATCH: the package at ${ARG_PREFIX} was built from ${_meta_revision}, the lock names ${_commit}")
            endif()
            if(NOT _meta_version STREQUAL _package_version)
                list(APPEND _problems
                    "TESSERA_LOCK_MISMATCH: the package version is ${_meta_version}, the lock names ${_package_version} (tag ${_tag})")
            endif()
        endif()
        ayther_tessera_package_api(_have_major _have_minor "${ARG_PREFIX}")
        if(NOT _have_major STREQUAL _api_major OR _have_minor LESS _api_minor)
            list(APPEND _problems
                "TESSERA_LOCK_MISMATCH: the package declares the API ${_have_major}.${_have_minor}, the lock names ${_api_major}.${_api_minor}")
        endif()
    endif()

    set("${out_problems}" "${_problems}" PARENT_SCOPE)
endfunction()
