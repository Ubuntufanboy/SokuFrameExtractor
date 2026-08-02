cmake_minimum_required(VERSION 3.21)
project(SokuFrameExtractor LANGUAGES CXX VERSION 0.2.0)

# ---------------------------------------------------------------------------
# This project cross-compiles a 32-bit Windows DLL from Linux using MSVC via
# msvc-wine.  MinGW is NOT a supported alternative: SokuLib exposes the game's
# C++ classes by hardcoded vtable layout and __thiscall/__fastcall member
# pointers, which MinGW's ABI does not reproduce byte-for-byte.  A MinGW build
# links but corrupts the game at runtime.
#
# Always configure through a preset:
#     cmake --preset msvc-wine
#     cmake --build --preset msvc-wine
# ---------------------------------------------------------------------------

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR
        "SokuFrameExtractor targets Windows (32-bit).  Configure with the "
        "bundled toolchain:\n"
        "    cmake --preset msvc-wine\n"
        "See README.md -> Building.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "Compiler is '${CMAKE_CXX_COMPILER_ID}', but MSVC is required.\n"
        "SokuLib's __thiscall/__fastcall vtable interop is ABI-specific; a "
        "MinGW build links successfully and then corrupts the game at "
        "runtime.  Use toolchains/msvc-wine.cmake (see README.md -> Building).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_subdirectory(dll)
