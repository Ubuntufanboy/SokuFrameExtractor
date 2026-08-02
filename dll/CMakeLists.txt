# ---------------------------------------------------------------------------
# SokuFrameExtractor.dll — SWRSToys module
# ---------------------------------------------------------------------------

# NOTE: SokuLib is deliberately NOT linked.
#
# The only thing this module ever used from it was SokuLib::sceneId, which is
# `*(Scene*)0x008A0044` -- a single dereference. Pulling the library in for
# that also pulled in parts that use iostreams (Packet.cpp's `stream <<`), and
# therefore msvcp140, whose Wine builtin aborts the process at module load.
# session.cpp reads the address directly instead.
#
# If a future change needs real SokuLib functionality, re-add
# `find_package(SokuLib REQUIRED)` and the link below -- but check the
# msvcp140 guard at the bottom of this file still passes.

add_library(SokuFrameExtractor SHARED
    src/main.cpp
    src/session.cpp
    src/logger.cpp
    src/video_encoder.cpp
    src/ring_buffer.cpp
    src/ogl_hook.cpp
)

target_include_directories(SokuFrameExtractor PRIVATE include)

target_link_libraries(SokuFrameExtractor PRIVATE
    shlwapi
)

target_compile_definitions(SokuFrameExtractor PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# SWRSToys loads the module by filename and resolves CheckVersion/Initialize
# by name, so the output must be exactly "SokuFrameExtractor.dll".
set_target_properties(SokuFrameExtractor PROPERTIES
    PREFIX ""
    OUTPUT_NAME "SokuFrameExtractor"
)

install(TARGETS SokuFrameExtractor
    RUNTIME DESTINATION "modules/SokuFrameExtractor"
    LIBRARY DESTINATION "modules/SokuFrameExtractor"
)
install(FILES "${CMAKE_SOURCE_DIR}/config/sfe.ini"
    DESTINATION "modules/SokuFrameExtractor"
    RENAME "SokuFrameExtractor.ini"
)

# ---------------------------------------------------------------------------
# Guard: the module must not import msvcp140
# ---------------------------------------------------------------------------
# Wine's builtin msvcp140.dll does not implement everything the MSVC STL calls
# (?_Throw_Cpp_error@std@@YAXH@Z, reached by std::mutex/std::thread), and
# calling it aborts the process during module load. The only evidence is a
# zero-byte log, which reads exactly like "the module never loaded" -- it cost
# a long debugging session to find once already.
#
# So this is enforced rather than remembered: any change that reintroduces
# <fstream>, <mutex>, <thread>, <filesystem> or std::string's throw helpers
# fails the build here instead of at runtime on a collection node.
find_program(SFE_OBJDUMP NAMES i686-w64-mingw32-objdump objdump)
if(SFE_OBJDUMP)
    add_custom_command(TARGET SokuFrameExtractor POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DDLL=$<TARGET_FILE:SokuFrameExtractor>
                -DOBJDUMP=${SFE_OBJDUMP}
                -P ${CMAKE_SOURCE_DIR}/cmake/CheckNoMsvcp.cmake
        VERBATIM)
else()
    message(WARNING
        "objdump not found -- skipping the msvcp140 import guard. "
        "The module may still build with a dependency that aborts under Wine.")
endif()
