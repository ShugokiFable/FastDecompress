cmake_minimum_required(VERSION 3.23)

set(NAME "FastDecompress")
set(VERSION 1.2.0)

message(">>> Start build for '${NAME}' v${VERSION}")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR ">> In-source builds not allowed.")
endif()

# Setup vcpkg BEFORE project()
macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)
if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
  set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
else()
  message(FATAL_ERROR ">> Variable VCPKG_ROOT is not set!")
endif()

# CommonLibF4 (alandtse fork — supports F4, NG, and VR in one build)
set(COMMONLIBF4_PATH "C:/Development/Community Shaders FO4/skyrim-community-shaders-dev/extern/CommonLibF4/CommonLibF4" CACHE STRING "")
if(NOT EXISTS "${COMMONLIBF4_PATH}/CMakeLists.txt")
  message(FATAL_ERROR ">> CommonLibF4 not found at ${COMMONLIBF4_PATH}")
endif()
message(">>> Using CommonLibF4 at: ${COMMONLIBF4_PATH}")

# Enable all three runtimes: F4 OG, NG, and VR
set(ENABLE_FALLOUT_F4 ON CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_NG ON CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "" FORCE)
set(F4SE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

project(${NAME} VERSION ${VERSION} LANGUAGES CXX C)
message(">>> Building project '${PROJECT_NAME}' v${PROJECT_VERSION}")

add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
    FALLOUT4
)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

add_subdirectory(${COMMONLIBF4_PATH} "CommonLibF4" EXCLUDE_FROM_ALL)

find_package(libdeflate CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)

# zlib-ng compat mode (built from source via FetchContent)
include(FetchContent)
FetchContent_Declare(
    zlibng
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG        2.1.5
)
set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_LIB OFF CACHE BOOL "" FORCE)
set(INSTALL_INC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlibng)
message(">>> zlib-ng compat mode built from source")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.h"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SOURCE_DIR}"
    "${zlibng_BINARY_DIR}"    # for generated zconf.h
    "${zlibng_SOURCE_DIR}"    # for zlib.h
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibF4::CommonLibF4
    libdeflate::libdeflate_static
    zlibstatic
    lz4::lz4
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(${PROJECT_NAME} PRIVATE
        /MP4
        /W3
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
    )
endif()

message(">>> ${PROJECT_NAME} configured successfully")

# =========================================================================
# ba2repack — standalone CLI tool to repack BA2 archives with LZ4
# =========================================================================

add_executable(ba2repack "tools/ba2repack/main.cpp")

target_compile_features(ba2repack PRIVATE cxx_std_23)

target_include_directories(ba2repack PRIVATE
    "${zlibng_BINARY_DIR}"
    "${zlibng_SOURCE_DIR}"
)

target_compile_definitions(ba2repack PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(ba2repack PRIVATE
    zlibstatic
    lz4::lz4
)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(ba2repack PRIVATE
        /MP4
        /W3
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
    )
endif()

message(">>> ba2repack configured successfully")
