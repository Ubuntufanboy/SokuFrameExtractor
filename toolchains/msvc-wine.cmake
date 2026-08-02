# ---------------------------------------------------------------------------
# CMake toolchain: 32-bit Windows target, MSVC running under Wine (msvc-wine).
# ---------------------------------------------------------------------------
# This is the toolchain that produced the known-good DLL.  It expects the
# msvc-wine wrapper scripts (cl, link, rc) to be on PATH -- see
# docker/build.Dockerfile, or scripts/msvc-env.sh for a host setup.
#
# Why MSVC and not MinGW: SokuLib binds the game's C++ objects through
# hardcoded vtable indices and __thiscall/__fastcall member function pointers.
# Those are MSVC ABI details.  MinGW compiles and links this project happily
# and then corrupts the game's vtables at runtime, which is a far worse
# failure mode than a build error -- hence the hard guard in CMakeLists.txt.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

# The msvc-wine wrappers present themselves as native MSVC tools, so CMake
# recognises them by name and sets the compiler ID correctly.
set(CMAKE_C_COMPILER   cl)
set(CMAKE_CXX_COMPILER cl)
set(CMAKE_RC_COMPILER  rc)

# Compiler probing otherwise tries to emit a PDB, which needs winbind and a
# separate rpcss service under Wine.  Embedded debug info sidesteps both.
# Requires CMake 3.25+.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")

# Look for libraries/headers in the target (Windows) sysroot only.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
