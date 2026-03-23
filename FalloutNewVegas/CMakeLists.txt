cmake_minimum_required(VERSION 3.23)

set(NAME "FastDecompressNV")
set(VERSION 1.2.0)

message(">>> Start build for '${NAME}' v${VERSION}")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR ">> In-source builds not allowed.")
endif()

# ── CRITICAL: FNV is 32-bit ─────────────────────────────────────────────────
# Must build as x86/Win32! Configure with: cmake -A Win32 ..
if(MSVC)
    if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        message(FATAL_ERROR "FNV is 32-bit! Configure with: cmake --preset vs2022-windows or cmake -A Win32 ..")
    endif()
endif()

project(${NAME} VERSION ${VERSION} LANGUAGES CXX C)
message(">>> Building project '${PROJECT_NAME}' v${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# ── Dependencies via FetchContent ────────────────────────────────────────────

include(FetchContent)

# libdeflate 1.25 (whole-buffer decompression for TESFile records)
FetchContent_Declare(
    libdeflate
    GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git
    GIT_TAG        v1.25
)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libdeflate)
message(">>> libdeflate v1.25 built from source")

# Chromium zlib — Google's SIMD-optimized zlib fork (powers Chrome)
# SSE2 chunk copy in inflate_fast, SIMD adler32/crc32
FetchContent_Declare(
    chromium_zlib
    GIT_REPOSITORY https://github.com/librepo/chromium-zlib.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(chromium_zlib)

# Build chromium-zlib as a static library with SIMD enabled for 32-bit
set(CZLIB "${chromium_zlib_SOURCE_DIR}")
add_library(chromium_zlib_static STATIC
    "${CZLIB}/adler32.c"
    "${CZLIB}/adler32_simd.c"
    "${CZLIB}/compress.c"
    "${CZLIB}/crc32.c"
    "${CZLIB}/crc32_simd.c"
    "${CZLIB}/cpu_features.c"
    "${CZLIB}/deflate.c"
    "${CZLIB}/gzclose.c"
    "${CZLIB}/gzlib.c"
    "${CZLIB}/gzread.c"
    "${CZLIB}/gzwrite.c"
    "${CZLIB}/infback.c"
    "${CZLIB}/inftrees.c"
    "${CZLIB}/trees.c"
    "${CZLIB}/uncompr.c"
    "${CZLIB}/zutil.c"
    # Chromium optimized inflate + inffast with SSE2 chunk copy
    "${CZLIB}/contrib/optimizations/inflate.c"
    "${CZLIB}/contrib/optimizations/inffast_chunk.c"
)
target_include_directories(chromium_zlib_static PUBLIC
    "${CZLIB}"
    "${CZLIB}/contrib/optimizations"
    "${chromium_zlib_BINARY_DIR}"  # for generated zconf.h
)
target_compile_definitions(chromium_zlib_static PRIVATE
    # SSE2 SIMD chunk copy in inflate — the key optimization
    INFLATE_CHUNK_SIMD_SSE2
    # INFLATE_CHUNK_READ_64LE — disabled, slower on 32-bit (emulated with two loads)
    # SIMD adler32 (SSSE3)
    ADLER32_SIMD_SSSE3
    # Platform detection for cpu_features.c
    X86_WINDOWS
    # Use optimized inflate_fast from contrib/optimizations
    INFLATE_CHUNK_GENERIC=0
    CHROMIUM_ZLIB_NO_CHROMECONF
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    target_compile_options(chromium_zlib_static PRIVATE /O2 /Oi /GL /W0)
endif()
message(">>> Chromium zlib built from source (SSE2 inflate, SSSE3 adler32)")

# ── Plugin sources ───────────────────────────────────────────────────────────

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.h"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SOURCE_DIR}"
    "${chromium_zlib_SOURCE_DIR}"  # for zlib.h, zconf.h
    "${libdeflate_SOURCE_DIR}"    # for libdeflate.h
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    libdeflate_static
    chromium_zlib_static
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    FASTDECOMPRESS_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(${PROJECT_NAME} PRIVATE
        /MP4
        /W3
        /permissive-
        /Zc:preprocessor
        /Zi
        /O2
        /Oi
        /GL
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG
        /OPT:REF
        /OPT:ICF
        /LTCG
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "zz_FastDecompressNV"
    PREFIX ""
    SUFFIX ".dll"
)

# ── Post-build: deploy to NVSE plugins directory ────────────────────────────
set(FNV_GAME_DIR "" CACHE PATH "Path to Fallout New Vegas game directory")
if(FNV_GAME_DIR AND EXISTS "${FNV_GAME_DIR}/FalloutNV.exe")
    set(DEPLOY_DIR "${FNV_GAME_DIR}/Data/NVSE/Plugins")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${DEPLOY_DIR}/zz_FastDecompressNV.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ROOT_DIR}/FastDecompressNV.ini"
            "${DEPLOY_DIR}/zz_FastDecompressNV.ini"
        COMMENT "Deploying FastDecompressNV to ${DEPLOY_DIR}")
    message(STATUS "Auto-deploy enabled: ${DEPLOY_DIR}")
endif()

message(">>> ${PROJECT_NAME} configured successfully")
