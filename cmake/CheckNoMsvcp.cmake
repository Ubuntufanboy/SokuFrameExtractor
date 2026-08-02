# Fails if the built DLL imports msvcp140. See the note in dll/CMakeLists.txt.
execute_process(
    COMMAND ${OBJDUMP} -p ${DLL}
    OUTPUT_VARIABLE out
    ERROR_QUIET
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(WARNING "msvcp140 guard: could not read imports of ${DLL}")
    return()
endif()

string(TOUPPER "${out}" out_upper)
if(out_upper MATCHES "DLL NAME: MSVCP140")
    # Pull the offending symbols out so the failure names what to remove.
    string(REGEX MATCHALL "\\?[A-Za-z_@$?0-9]+" syms "${out}")
    list(LENGTH syms n)
    message(FATAL_ERROR
        "SokuFrameExtractor.dll imports MSVCP140.dll.\n"
        "Wine's builtin msvcp140 aborts the process at module load "
        "(?_Throw_Cpp_error). Replace the offending C++ standard library use "
        "with Win32/UCRT equivalents:\n"
        "  <mutex>/<condition_variable> -> CRITICAL_SECTION + CONDITION_VARIABLE\n"
        "  <thread>                     -> CreateThread\n"
        "  <fstream>                    -> FILE* (stdio)\n"
        "  <filesystem>                 -> CreateDirectoryA / FindFirstFileA\n"
        "  std::string                  -> fixed char buffers\n"
        "See dll/include/sfe/config.hpp for the full rationale.")
endif()
message(STATUS "msvcp140 guard: OK (no MSVCP140 import)")
