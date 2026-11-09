# SPDX-License-Identifier: GPL-3.0-or-later
# Choose the Python interpreter the launcher runs as root through pkexec.
#
# The interpreter must pass the same rule as the helper scripts (LauncherMount::trustedFile and
# require_root_owned in tools/mount-cryptex.py): every component of its path, before and after
# resolving symlinks, is owned by root (or EXTRA_OWNER, for tests), and none is group- or
# world-writable (a symlink only needs the ownership, its target chain is checked too). It must also
# run and report a version of at least MINIMUM. There is no fallback to PATH or to an untrusted result.
#
# launcher_select_python(<result> <error>
#     [OVERRIDE <path>] MINIMUM <x.y> [EXTRA_OWNER <uid>] CANDIDATES <path>...)
# sets <result> to the chosen absolute path, or <error> to a message listing every candidate tried.
# With OVERRIDE only that path is considered.
#
# launcher_resolve_python(<result> <level> <text> ...same arguments...) turns that into the configure outcome:
#   found                      <result> = path, <level> empty
#   explicit OVERRIDE fails    <result> empty, <level> FATAL_ERROR (the user asked for it)
#   nothing qualifies          <result> empty, <level> WARNING (the launcher builds, the root-run actions stay disabled)

set(LAUNCHER_STAT_TOOL "/usr/bin/stat")

# Sets <out> to the reason the path is not trusted, or leaves it empty.
function(_launcher_chain_problem path extra_owner out)
    set(${out} "" PARENT_SCOPE)
    string(REPLACE "/" ";" parts "${path}")
    set(current "")
    foreach(part IN LISTS parts)
        if(part STREQUAL "")
            continue()
        endif()
        string(APPEND current "/${part}")
        execute_process(COMMAND "${LAUNCHER_STAT_TOOL}" -c "%u %A" "${current}"
                        OUTPUT_VARIABLE info RESULT_VARIABLE status ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT status EQUAL 0 OR NOT info MATCHES "^([0-9]+) ([-a-z])([-rwxsStT]+)$")
            set(${out} "cannot inspect ${current}" PARENT_SCOPE)
            return()
        endif()
        set(owner "${CMAKE_MATCH_1}")
        set(kind "${CMAKE_MATCH_2}")
        string(SUBSTRING "${CMAKE_MATCH_3}" 4 1 group_write)
        string(SUBSTRING "${CMAKE_MATCH_3}" 7 1 other_write)
        if(NOT owner EQUAL 0 AND NOT (NOT extra_owner STREQUAL "" AND owner EQUAL extra_owner))
            set(${out} "${current} is not owned by root" PARENT_SCOPE)
            return()
        endif()
        # Linux reports 0777 for every symlink; the resolved chain is checked separately.
        if(NOT kind STREQUAL "l" AND (group_write STREQUAL "w" OR other_write STREQUAL "w"))
            set(${out} "${current} is writable by group or others" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Sets <out> to the reason the candidate cannot be used, or leaves it empty.
function(_launcher_candidate_problem candidate minimum extra_owner out)
    set(${out} "" PARENT_SCOPE)
    if(NOT IS_ABSOLUTE "${candidate}")
        set(${out} "not an absolute path" PARENT_SCOPE)
        return()
    endif()
    cmake_path(NORMAL_PATH candidate OUTPUT_VARIABLE lexical)
    if(NOT EXISTS "${lexical}")
        set(${out} "does not exist" PARENT_SCOPE)
        return()
    endif()
    file(REAL_PATH "${lexical}" resolved)
    foreach(chain "${lexical}" "${resolved}")
        _launcher_chain_problem("${chain}" "${extra_owner}" reason)
        if(NOT reason STREQUAL "")
            set(${out} "${reason}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    execute_process(COMMAND "${LAUNCHER_STAT_TOOL}" -L -c "%A" "${lexical}" OUTPUT_VARIABLE mode
                    RESULT_VARIABLE status ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status EQUAL 0 OR NOT mode MATCHES "^-.*x")
        set(${out} "not an executable regular file" PARENT_SCOPE)
        return()
    endif()
    execute_process(COMMAND "${lexical}" -I -c "import sys; print('%d.%d' % sys.version_info[:2])"
                    OUTPUT_VARIABLE version RESULT_VARIABLE status ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 20)
    if(NOT status EQUAL 0 OR NOT version MATCHES "^[0-9]+\\.[0-9]+$")
        set(${out} "did not report a Python version" PARENT_SCOPE)
        return()
    endif()
    if(version VERSION_LESS minimum)
        set(${out} "is Python ${version}, older than ${minimum}" PARENT_SCOPE)
    endif()
endfunction()

function(launcher_select_python result error)
    if(POLICY CMP0174)
        cmake_policy(SET CMP0174 NEW)  # an empty OVERRIDE is a defined empty value, not a warning
    endif()
    # PARSE_ARGV keeps an empty OVERRIDE ("" is the normal no-override value); expanding ${ARGN} would drop it.
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "OVERRIDE;MINIMUM;EXTRA_OWNER" "CANDIDATES")
    set(${result} "" PARENT_SCOPE)
    set(${error} "" PARENT_SCOPE)
    if(ARG_OVERRIDE)
        set(candidates "${ARG_OVERRIDE}")
    else()
        set(candidates ${ARG_CANDIDATES})
        list(REMOVE_DUPLICATES candidates)
    endif()
    set(tried "")
    foreach(candidate IN LISTS candidates)
        _launcher_candidate_problem("${candidate}" "${ARG_MINIMUM}" "${ARG_EXTRA_OWNER}" reason)
        if(reason STREQUAL "")
            set(${result} "${candidate}" PARENT_SCOPE)
            return()
        endif()
        string(APPEND tried "\n  ${candidate}: ${reason}")
    endforeach()
    if(ARG_OVERRIDE)
        set(${error} "LAUNCHER_PYTHON=${ARG_OVERRIDE} cannot be used as the root interpreter:${tried}" PARENT_SCOPE)
    else()
        set(${error} "No trusted Python ${ARG_MINIMUM} or newer for the root-run mount scripts. Tried:${tried}\nInstall a system Python (root-owned, not group- or world-writable along its whole path) or pass -DLAUNCHER_PYTHON=<path> to a trusted one; interpreters in a home directory or on PATH are never used." PARENT_SCOPE)
    endif()
endfunction()

function(launcher_resolve_python result level text)
    if(POLICY CMP0174)
        cmake_policy(SET CMP0174 NEW)  # an empty OVERRIDE is a defined empty value, not a warning
    endif()
    cmake_parse_arguments(PARSE_ARGV 3 ARG "" "OVERRIDE;MINIMUM;EXTRA_OWNER" "CANDIDATES")
    launcher_select_python(chosen problem OVERRIDE "${ARG_OVERRIDE}" MINIMUM "${ARG_MINIMUM}" EXTRA_OWNER "${ARG_EXTRA_OWNER}"
                           CANDIDATES ${ARG_CANDIDATES})
    set(${result} "${chosen}" PARENT_SCOPE)
    set(${level} "" PARENT_SCOPE)
    set(${text} "" PARENT_SCOPE)
    if(NOT chosen STREQUAL "")
        return()
    endif()
    if(ARG_OVERRIDE)
        set(${level} "FATAL_ERROR" PARENT_SCOPE)
        set(${text} "${problem}" PARENT_SCOPE)
    else()
        set(${level} "WARNING" PARENT_SCOPE)
        set(${text} "${problem}\nThe launcher is built without a root-run interpreter: the cryptex/Preboot mount actions stay disabled in the GUI until it is reconfigured (see the README)." PARENT_SCOPE)
    endif()
endfunction()
