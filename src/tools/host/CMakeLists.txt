cmake_minimum_required(VERSION 3.30)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(quake2_tools)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} CACHE INTERNAL "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARINGS _USE_MATH_DEFINES strcasecmp=_stricmp)
endif()

if(MSVC)
    include(FetchContent)

    FetchContent_Declare(
        dirent
        URL https://github.com/tronkko/dirent/archive/refs/tags/1.26.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(dirent)
endif()

set(BSPINFO_SOURCES bspinfo.cpp)

add_executable(bspinfo ${BSPINFO_SOURCES})

if(MSVC)
    target_compile_definitions(bspinfo PRIVATE
        _USE_MATH_DEFINES
        strcasecmp=_stricmp
    )
	
    target_include_directories(bspinfo PRIVATE
        "${dirent_SOURCE_DIR}/include"
    )
	
	target_link_libraries(bspinfo dirent)	
endif()

set(IMGDUMP_SOURCES imgdump.cpp)

add_executable(imgdump ${IMGDUMP_SOURCES})

set(MUSENC_SOURCES musenc.cpp)

add_executable(musenc ${MUSENC_SOURCES})

set(PAKZIP_SOURCES pakzip.cpp)

add_executable(pakzip ${PAKZIP_SOURCES})

add_subdirectory(../miniz miniz_build)

target_link_libraries(pakzip miniz)

target_include_directories(pakzip PRIVATE ../miniz)

set(UNPAK_SOURCES unpak.cpp)

add_executable(unpak ${UNPAK_SOURCES})

if(MSVC)
    target_include_directories(unpak PRIVATE
        "${dirent_SOURCE_DIR}/include"
    )
	
	target_link_libraries(unpak dirent)
endif()

