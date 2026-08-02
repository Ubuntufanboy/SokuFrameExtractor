# ---------------------------------------------------------------------------
# FindSokuLib.cmake
# ---------------------------------------------------------------------------
# Locates SokuLib and exposes it as the imported target `SokuLib::SokuLib`.
#
# Resolution order:
#   1. -DSOKULIB_DIR=<path>            (explicit override)
#   2. $ENV{SOKULIB_DIR}               (used by docker/build.Dockerfile)
#   3. third_party/SokuLib             (the pinned git submodule — preferred)
#
# The old CMakeLists guessed at ../SokuLib and lib/SokuLib, which silently
# picked up whatever happened to be next to the checkout.  That is how the
# working build ended up depending on ~/soku-build/SokuLib with an
# uncommitted local patch.  Resolution is now explicit and reported.
#
# NOTE: SokuLib upstream defines test targets unconditionally, and those do not
# cross-compile cleanly.  We only ever build our own target by name, so they
# are declared-but-never-built.  cmake/patches/sokulib-optional-tests.patch
# gates them behind SOKULIB_BUILD_TESTS for anyone who wants `--build .` on the
# whole tree to work; applying it is optional.
# ---------------------------------------------------------------------------

if(TARGET SokuLib::SokuLib)
    return()
endif()

set(_sokulib_candidates)
if(SOKULIB_DIR)
    list(APPEND _sokulib_candidates "${SOKULIB_DIR}")
endif()
if(DEFINED ENV{SOKULIB_DIR})
    list(APPEND _sokulib_candidates "$ENV{SOKULIB_DIR}")
endif()
list(APPEND _sokulib_candidates "${CMAKE_SOURCE_DIR}/third_party/SokuLib")

set(_sokulib_root "")
foreach(_cand IN LISTS _sokulib_candidates)
    if(EXISTS "${_cand}/src/SokuLib.hpp")
        set(_sokulib_root "${_cand}")
        break()
    endif()
endforeach()

if(NOT _sokulib_root)
    message(FATAL_ERROR
        "SokuLib not found.  Looked for src/SokuLib.hpp under:\n"
        "  ${_sokulib_candidates}\n"
        "Fetch the pinned submodule:\n"
        "    git submodule update --init --recursive\n"
        "or point at an existing checkout:\n"
        "    cmake --preset msvc-wine -DSOKULIB_DIR=/path/to/SokuLib")
endif()

get_filename_component(_sokulib_root "${_sokulib_root}" ABSOLUTE)
message(STATUS "SokuLib: ${_sokulib_root}")

if(NOT EXISTS "${_sokulib_root}/CMakeLists.txt")
    message(FATAL_ERROR "SokuLib at ${_sokulib_root} has no CMakeLists.txt")
endif()

set(SOKULIB_BUILD_TESTS OFF CACHE BOOL "Build SokuLib's own test targets" FORCE)
add_subdirectory("${_sokulib_root}" "${CMAKE_BINARY_DIR}/SokuLib" EXCLUDE_FROM_ALL)

add_library(SokuLib::SokuLib ALIAS SokuLib)
set(SokuLib_FOUND TRUE)
set(SokuLib_ROOT "${_sokulib_root}")
