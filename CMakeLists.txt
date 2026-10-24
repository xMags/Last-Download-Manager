cmake_minimum_required(VERSION 3.30)

project(LastDownloadManager
    VERSION 5.0.1
    DESCRIPTION "Native Windows download manager"
    LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Last Download Manager builds only with MSVC on Windows.")
endif()

option(LDM_BUILD_TESTS "Build unit and integration tests" ON)
option(LDM_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(LDM_ENABLE_ANALYZE "Build with MSVC code analysis (/analyze)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static runtime, matching the x64-windows-static vcpkg triplet.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Shared compile settings for every LDM target. Third-party headers are treated as external
# so their warnings never fail the build.
add_library(ldm_build_settings INTERFACE)
target_compile_options(ldm_build_settings INTERFACE
    /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
    /external:anglebrackets /external:W0
    $<$<CONFIG:Release>:/Zi>)
target_compile_definitions(ldm_build_settings INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX STRICT
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000008)
target_link_options(ldm_build_settings INTERFACE
    $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

if(LDM_ENABLE_ASAN)
    target_compile_options(ldm_build_settings INTERFACE /fsanitize=address /Zi)
    target_link_options(ldm_build_settings INTERFACE /INCREMENTAL:NO)
endif()
if(LDM_ENABLE_ANALYZE)
    target_compile_options(ldm_build_settings INTERFACE /analyze /analyze:external-)
endif()

if(LDM_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
add_subdirectory(extension)
include(cmake/Packaging.cmake)
include(cmake/Installer.cmake)

if(LDM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
