# SPDX-License-Identifier: GPL-3.0-or-later
# Run with: cmake -P tests/select_python_test.cmake
# Fake interpreters live in a tree under HOME owned by the test user, which stands in for root (EXTRA_OWNER).
cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/SelectPython.cmake")

execute_process(COMMAND id -u OUTPUT_VARIABLE me OUTPUT_STRIP_TRAILING_WHITESPACE)
string(RANDOM LENGTH 8 ALPHABET "abcdefghijklmnopqrstuvwxyz" suffix)
file(REAL_PATH "$ENV{HOME}" home)
set(tree "${home}/.launcher-python-select-${suffix}")
file(MAKE_DIRECTORY "${tree}/bin" "${tree}/loose")
file(CHMOD "${tree}" "${tree}/bin" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(CHMOD "${tree}/loose" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_WRITE GROUP_EXECUTE)

function(check condition message)
    # The condition keeps its quotes only when evaluated as code, not when expanded into if(...) arguments.
    set(check_message "${message}")
    cmake_language(EVAL CODE "if(NOT (${condition}))\n    message(SEND_ERROR \"FAILED: \${check_message}\")\nendif()")
endfunction()

function(fake directory name body)
    file(WRITE "${directory}/${name}" "#!/bin/sh\n${body}\n")
    file(CHMOD "${directory}/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endfunction()

fake("${tree}/bin" py312 "echo 3.12")
fake("${tree}/bin" py39 "echo 3.9")
fake("${tree}/bin" py37 "echo 3.7")
fake("${tree}/bin" broken "exit 1")
fake("${tree}/bin" noversion "echo hello")
fake("${tree}/loose" pyloose "echo 3.12")
file(WRITE "${tree}/bin/plain" "data")
file(CREATE_LINK "${tree}/bin/py312" "${tree}/bin/link312" SYMBOLIC)
file(CREATE_LINK "${tree}/loose/pyloose" "${tree}/bin/linkloose" SYMBOLIC)
set(bin "${tree}/bin")

# 1. a trusted interpreter is chosen
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/py312")
check("r STREQUAL \"${bin}/py312\" AND e STREQUAL \"\"" "trusted candidate accepted: '${r}' '${e}'")

# 2. not owned by root: refused, no result, nothing silently substituted
launcher_select_python(r e MINIMUM 3.8 CANDIDATES "${bin}/py312")
check("r STREQUAL \"\"" "user-owned interpreter must not be chosen")
check("e MATCHES \"not owned by root\"" "ownership reason reported: ${e}")

# 3. group-writable directory or file
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${tree}/loose/pyloose")
check("r STREQUAL \"\" AND e MATCHES \"writable by group or others\"" "group-writable parent refused: '${r}' ${e}")

# 4. too old: skipped, the next candidate wins, order is respected
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/py37" "${bin}/py39" "${bin}/py312")
check("r STREQUAL \"${bin}/py39\"" "too-old candidate skipped, first valid one wins: '${r}'")
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/py37")
check("r STREQUAL \"\" AND e MATCHES \"older than 3.8\"" "old version reported: ${e}")
launcher_select_python(r e MINIMUM 3.10 EXTRA_OWNER ${me} CANDIDATES "${bin}/py39")
check("r STREQUAL \"\"" "minimum is honoured")

# 5. not runnable, no version, not a file, missing, relative
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/broken" "${bin}/noversion" "${bin}/plain" "${bin}/absent" "python3")
check("r STREQUAL \"\"" "unusable candidates refused")
check("e MATCHES \"did not report a Python version\" AND e MATCHES \"not an executable regular file\" AND e MATCHES \"does not exist\" AND e MATCHES \"not an absolute path\"" "every candidate listed with its reason: ${e}")
check("e MATCHES \"never used\"" "message says PATH and home interpreters are never used")

# 6. symlinks: root-owned link to a trusted target is fine; a link to a writable target is not
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/link312")
check("r STREQUAL \"${bin}/link312\"" "symlink to a trusted target accepted: '${r}' ${e}")
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/linkloose")
check("r STREQUAL \"\" AND e MATCHES \"writable by group or others\"" "symlink to a writable target refused: '${r}' ${e}")

# 7. an override is validated by the same rule and is the only candidate
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} OVERRIDE "${bin}/py312" CANDIDATES "${bin}/py39")
check("r STREQUAL \"${bin}/py312\"" "override honoured")
launcher_select_python(r e MINIMUM 3.8 EXTRA_OWNER ${me} OVERRIDE "${tree}/loose/pyloose" CANDIDATES "${bin}/py312")
check("r STREQUAL \"\" AND e MATCHES \"LAUNCHER_PYTHON=\"" "untrusted override fails instead of falling back: '${r}' ${e}")

# 8. the real system interpreter, when present, passes the same rule without EXTRA_OWNER
if(EXISTS /usr/bin/python3)
    launcher_select_python(r e MINIMUM 3.8 CANDIDATES /usr/bin/python3)
    check("r STREQUAL \"/usr/bin/python3\"" "system python3 is trusted: ${e}")
endif()

# 9. configure outcomes: found, explicit override fails (fatal), nothing qualifies (warn, empty, no fallback)
launcher_resolve_python(r level text MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/py312")
check("r STREQUAL \"${bin}/py312\" AND level STREQUAL \"\" AND text STREQUAL \"\"" "found: no message, path baked: '${r}' '${level}'")
launcher_resolve_python(r level text MINIMUM 3.8 CANDIDATES "${bin}/py312" "${bin}/py39")
check("r STREQUAL \"\" AND level STREQUAL \"WARNING\"" "nothing qualifies without an override only warns: '${r}' '${level}'")
check("text MATCHES \"not owned by root\" AND text MATCHES \"py312\" AND text MATCHES \"py39\" AND text MATCHES \"stay disabled\" AND text MATCHES \"README\"" "warning lists what was tried and says what is disabled: ${text}")
launcher_resolve_python(r level text MINIMUM 3.8 OVERRIDE "${bin}/py312" CANDIDATES "${bin}/py312")
check("r STREQUAL \"\" AND level STREQUAL \"FATAL_ERROR\"" "an explicit override that fails is fatal, not a warning: '${r}' '${level}'")
check("text MATCHES \"LAUNCHER_PYTHON=\"" "fatal text names the variable: ${text}")
launcher_resolve_python(r level text MINIMUM 3.8 OVERRIDE "${bin}/py37" EXTRA_OWNER ${me} CANDIDATES "${bin}/py312")
check("r STREQUAL \"\" AND level STREQUAL \"FATAL_ERROR\"" "a too-old override is fatal and does not fall back to the candidates")
launcher_resolve_python(r level text MINIMUM 3.8 OVERRIDE "" EXTRA_OWNER ${me} CANDIDATES "${bin}/py37" "${bin}/py312")
check("r STREQUAL \"${bin}/py312\" AND level STREQUAL \"\"" "an empty override means automatic selection")

# 10. the argument order CMakeLists.txt uses, with the empty override of a normal configure
launcher_resolve_python(r level text OVERRIDE "" MINIMUM 3.8 CANDIDATES "${bin}/py312" "${bin}/py39")
check("r STREQUAL \"\" AND level STREQUAL \"WARNING\"" "empty override and nothing trusted: warning, not fatal: '${r}' '${level}'")
launcher_resolve_python(r level text OVERRIDE "" MINIMUM 3.8 EXTRA_OWNER ${me} CANDIDATES "${bin}/py37" "${bin}/py39")
check("r STREQUAL \"${bin}/py39\" AND level STREQUAL \"\"" "empty override and a qualifying candidate: chosen quietly: '${r}' '${level}'")

file(REMOVE_RECURSE "${tree}")
message(STATUS "select_python_test finished")
