# ================================================================================================
# The host command line tools (README.md, "Host tools"): imgdump, unpak, bspinfo, musenc and
# pakzip, built with the host's own C++ compiler on macOS, Linux or Windows (MSVC). `make tools`
# drives this project on macOS and Linux. Anywhere else, from the repository root:
#
#   cmake -S src/tools/host -B build/tools
#   cmake --build build/tools --config Release
#
# The binaries land in the build directory (in a per-config folder under Visual Studio or Xcode).
# ================================================================================================

cmake_minimum_required(VERSION 3.24)
project(quake2_tools C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Always optimized: pakzip deflates whole packs, and musenc tries every ADPCM filter and shift
# on every block. A single-config generator given no build type would not optimize at all.
get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release, RelWithDebInfo, Debug or MinSizeRel." FORCE)
endif()

# MSVC links its C/C++ runtime statically (/MT), so each .exe runs on its own, without the
# Visual C++ Redistributable installed. Set before any target is made, miniz included: a
# runtime mismatch between objects is a link error.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The binaries go to the build directory itself, unless the caller names another place (the
# Makefile does: build/tools/, beside oxide-vcl and symbolize, with this build tree under it).
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# ------------------------------------------------------------------------------------------------
# Warnings
# ------------------------------------------------------------------------------------------------

# The strict set the game's C++ is held to (EE_CXX_WARNFLAGS in the Makefile), as errors, with
# -Wpedantic added, since MSVC builds these too and has none of the GNU extensions. Without
# -Wdouble-promotion: it guards the EE's missing double hardware, and the tools do their signal
# math in double. MSVC gets /W4. To get past a warning a newer compiler adds, configure with
# --compile-no-warning-as-error.
if(MSVC)
    set(TOOL_WARNINGS /W4)
else()
    set(TOOL_WARNINGS
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wformat=2 -Wno-format-nonliteral -Wundef -Wpointer-arith
        -Wcast-align -Wwrite-strings -Wredundant-decls -Wnull-dereference
        -Wnon-virtual-dtor -Woverloaded-virtual -Wvla)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND TOOL_WARNINGS -Wlogical-op -Wduplicated-cond -Wduplicated-branches)
    endif()
endif()

# Each tool is a single self-contained .cpp.
function(add_tool name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE ${TOOL_WARNINGS})
    set_target_properties(${name} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
    if(MSVC)
        # The portable C runtime calls (fopen, snprintf...) rather than the _s ones, M_PI,
        # and POSIX's strcasecmp.
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES strcasecmp=_stricmp)
    endif()
endfunction()

# ------------------------------------------------------------------------------------------------
# Tools
# ------------------------------------------------------------------------------------------------

add_tool(bspinfo)
add_tool(imgdump)
add_tool(musenc)
add_tool(pakzip)
add_tool(unpak)

# miniz for pakzip, built the way the game builds it: only the raw deflate/inflate and CRC-32
# sources, straight from the submodule, as C11, and its warnings are not ours to fix. Not
# through miniz's own CMakeLists.txt, which adds the zip API and generates a miniz_export.h of
# its own. The one in miniz_cfg/ stands in for that header and carries the host configuration:
# the full-size compressor tables the game trims (TDEFL_LESS_MEMORY), and no stdio or zlib API.
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../miniz)
if(NOT EXISTS ${MINIZ_DIR}/miniz.c)
    message(FATAL_ERROR "${MINIZ_DIR} is empty - run 'git submodule update --init'")
endif()

add_library(miniz STATIC ${MINIZ_DIR}/miniz.c ${MINIZ_DIR}/miniz_tdef.c ${MINIZ_DIR}/miniz_tinfl.c)
set_target_properties(miniz PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(miniz SYSTEM PUBLIC ${MINIZ_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/miniz_cfg)
if(MSVC)
    # /W0, not /w: Visual Studio projects map /W0 onto their warning level setting, while /w
    # is added after the default /W1 and draws a D9025 "overriding" warning.
    target_compile_options(miniz PRIVATE /W0)
else()
    target_compile_options(miniz PRIVATE -w)
endif()

target_link_libraries(pakzip PRIVATE miniz)

# MSVC has no <dirent.h>: bspinfo walks directories with it, and unpak takes S_ISDIR from it.
# tronkko/dirent implements it over the Win32 API, in one header. Included as a system header:
# it pulls in <windows.h>, and neither is ours to hold to /W4.
if(MSVC)
    include(FetchContent)
    FetchContent_Declare(dirent
        URL https://github.com/tronkko/dirent/archive/refs/tags/1.26.zip
        URL_HASH SHA256=6ffcc318f00be192acb611c58aa58ee0cfd96776010680d7f38cf24f3dd8baf9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(dirent)

    target_include_directories(bspinfo SYSTEM PRIVATE ${dirent_SOURCE_DIR}/include)
    target_include_directories(unpak SYSTEM PRIVATE ${dirent_SOURCE_DIR}/include)
endif()
