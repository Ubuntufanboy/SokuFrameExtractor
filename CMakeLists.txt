cmake_minimum_required(VERSION 3.15)
project(SokuFrameExtractor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# SokuLib must be available. Set SOKULIB_DIR to the SokuLib root.
# Example: cmake -DSOKULIB_DIR=C:/dev/SokuLib ..
if(NOT DEFINED SOKULIB_DIR)
    # Try common relative paths
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../SokuLib")
        set(SOKULIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SokuLib")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/SokuLib")
        set(SOKULIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SokuLib")
    else()
        message(FATAL_ERROR
            "SokuLib not found. Set SOKULIB_DIR:\n"
            "  cmake -DSOKULIB_DIR=<path/to/SokuLib> .."
        )
    endif()
endif()

message(STATUS "Using SokuLib from: ${SOKULIB_DIR}")

# SokuLib headers
include_directories("${SOKULIB_DIR}/src")
include_directories("${SOKULIB_DIR}/directx")

# SokuLib static library (build it if a CMakeLists exists, else link .lib)
if(EXISTS "${SOKULIB_DIR}/CMakeLists.txt")
    add_subdirectory("${SOKULIB_DIR}" "${CMAKE_BINARY_DIR}/SokuLib")
    set(SOKULIB_TARGET SokuLib)
else()
    # Fallback: assume pre-built .lib
    find_library(SOKULIB_LIB SokuLib PATHS "${SOKULIB_DIR}/lib" "${SOKULIB_DIR}/build")
    if(NOT SOKULIB_LIB)
        message(FATAL_ERROR "Could not find SokuLib.lib — build SokuLib first")
    endif()
    add_library(SokuLib STATIC IMPORTED)
    set_target_properties(SokuLib PROPERTIES IMPORTED_LOCATION "${SOKULIB_LIB}")
    set(SOKULIB_TARGET SokuLib)
endif()

# DirectX 9 SDK (Windows SDK includes d3d9)
find_library(D3D9_LIB d3d9)
find_library(D3DX9_LIB d3dx9)

# ---------------------------------------------------------------------------
# SokuFrameExtractor DLL
# ---------------------------------------------------------------------------
add_library(SokuFrameExtractor SHARED
    src/main.cpp
    src/frame_extractor.cpp
    src/logger.cpp
    src/video_encoder.cpp
    src/ring_buffer.cpp
    src/ogl_hook.cpp
)

target_link_libraries(SokuFrameExtractor PRIVATE
    ${SOKULIB_TARGET}
    d3d9
    shlwapi
)

# SWRSToys modules must export CheckVersion and Initialize with C linkage
target_compile_definitions(SokuFrameExtractor PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# Output the DLL without the "lib" prefix on MinGW
set_target_properties(SokuFrameExtractor PROPERTIES PREFIX "")

# Install to a modules directory
install(TARGETS SokuFrameExtractor
    RUNTIME DESTINATION "modules/SokuFrameExtractor"
    LIBRARY DESTINATION "modules/SokuFrameExtractor"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/SokuFrameExtractor.ini"
    DESTINATION "modules/SokuFrameExtractor"
    OPTIONAL
)