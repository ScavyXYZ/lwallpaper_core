cmake_minimum_required(VERSION 3.21)
project(livewallpaper LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "livewallpaper currently only supports Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "livewallpaper requires a 64-bit toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Hide the dependency headers from anything that links this library.
set(CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE ON)

# ---------------------------------------------------------------------------
# Dependencies
#
# Both are expected as plain "devel" trees rather than installed packages. Point
# FFMPEG_ROOT / SDL3_ROOT at them, or leave them unset and let the search below
# find the copies vendored in the repository.
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "" CACHE PATH "ffmpeg tree containing include/ and lib/")
set(SDL3_ROOT   "" CACHE PATH "SDL3 VC tree containing include/ and lib/")

function(_lwp_find_subtree out_var subdir)
    set(candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${subdir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/lwallpaper/${subdir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/${subdir}")
    foreach(c IN LISTS candidates)
        if(EXISTS "${c}")
            set(${out_var} "${c}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

if(NOT FFMPEG_ROOT)
    _lwp_find_subtree(_ffmpeg_candidate ffmpeg)
    set(FFMPEG_ROOT "${_ffmpeg_candidate}")
endif()
if(NOT SDL3_ROOT)
    _lwp_find_subtree(_sdl3_candidate SDL3-devel-3.4.2-VC/SDL3-3.4.2)
    set(SDL3_ROOT "${_sdl3_candidate}")
endif()

if(NOT FFMPEG_ROOT OR NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "Could not find the ffmpeg headers. Set -DFFMPEG_ROOT=<path> to the tree "
        "containing include/ and lib/.")
endif()
if(NOT SDL3_ROOT OR NOT EXISTS "${SDL3_ROOT}/include/SDL3/SDL.h")
    message(FATAL_ERROR
        "Could not find the SDL3 headers. Set -DSDL3_ROOT=<path> to the tree "
        "containing include/ and lib/.")
endif()

# find_library resolves a single name, so ask for each ffmpeg component
# separately; a single import library is not enough to satisfy the API we use.
set(FFMPEG_LIBS "")
foreach(_comp IN ITEMS avcodec avformat avutil swscale swresample avdevice avfilter)
    find_library(FFMPEG_LIB_${_comp}
        NAMES ${_comp}
        PATHS "${FFMPEG_ROOT}/lib" "${FFMPEG_ROOT}/lib/x64"
        NO_DEFAULT_PATH)
    if(NOT FFMPEG_LIB_${_comp})
        message(FATAL_ERROR "Could not find ffmpeg component '${_comp}' under ${FFMPEG_ROOT}/lib")
    endif()
    list(APPEND FFMPEG_LIBS "${FFMPEG_LIB_${_comp}}")
endforeach()
unset(_comp)
message(STATUS "ffmpeg libs: ${FFMPEG_LIBS}")

find_library(SDL3_LIB
    NAMES SDL3
    PATHS "${SDL3_ROOT}/lib/x64" "${SDL3_ROOT}/lib"
    NO_DEFAULT_PATH REQUIRED)

message(STATUS "ffmpeg: ${FFMPEG_ROOT}")
message(STATUS "SDL3:   ${SDL3_ROOT}")

# SDL3 is built with both x86 and x64 folders; keep the import lib's own
# architecture check to catch an accidental 32-bit link.
# Convenience bundle for our own targets (the example). Not exported: the
# library below records its dependencies by plain path instead, so consumers
# never need ffmpeg's or SDL3's headers - include/livewallpaper/livewallpaper.hpp
# is free of both.
add_library(lwp_deps INTERFACE)
target_include_directories(lwp_deps SYSTEM INTERFACE
    "${FFMPEG_ROOT}/include"
    "${SDL3_ROOT}/include")
target_link_libraries(lwp_deps INTERFACE
    "${FFMPEG_LIBS}"
    "${SDL3_LIB}"
    d3d11 dxgi d3dcompiler winmm)

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
add_library(livewallpaper STATIC
    src/log.cpp
    src/desktop_window.cpp
    src/decoder.cpp
    src/zero_copy_renderer.cpp
    src/sdl_renderer.cpp
    src/transcode.cpp
    src/wallpaper.cpp)

target_include_directories(livewallpaper
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ffmpeg's headers are not /W4 clean, so keep them out of our warning set.
target_include_directories(livewallpaper SYSTEM PRIVATE
    "${FFMPEG_ROOT}/include"
    "${SDL3_ROOT}/include")

# A static library's PRIVATE link libraries still propagate to consumers as
# link-only requirements, which is what we want: they get the import libraries
# but never see ffmpeg's or SDL3's headers.
target_link_libraries(livewallpaper PRIVATE
    "${FFMPEG_LIBS}" "${SDL3_LIB}" d3d11 dxgi d3dcompiler winmm)
target_compile_definitions(livewallpaper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if(MSVC)
    target_compile_options(livewallpaper PRIVATE /W4 /utf-8)
endif()

# ---------------------------------------------------------------------------
# Example
# ---------------------------------------------------------------------------
add_executable(livewallpaper_example examples/play.cpp)
target_link_libraries(livewallpaper_example PRIVATE livewallpaper lwp_deps)
target_compile_definitions(livewallpaper_example PRIVATE UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# Self test: covers stop / restart / error paths that a screenshot cannot
# ---------------------------------------------------------------------------
add_executable(livewallpaper_selftest tests/selftest.cpp)
target_link_libraries(livewallpaper_selftest PRIVATE livewallpaper lwp_deps)
target_compile_definitions(livewallpaper_selftest PRIVATE UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# Install / package config so consumers can `find_package(livewallpaper)`
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS livewallpaper
        EXPORT livewallpaperTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT livewallpaperTargets
        FILE livewallpaperTargets.cmake
        NAMESPACE livewallpaper::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/livewallpaper)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfigVersion.cmake"
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfig.cmake"
"include(CMakeFindDependencyMacro)\n\
include(\"\${CMAKE_CURRENT_LIST_DIR}/livewallpaperTargets.cmake\")\n")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/livewallpaper)
