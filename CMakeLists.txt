cmake_minimum_required(VERSION 3.21)
project(livewallpaper VERSION 1.1.0 LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "livewallpaper requires Windows and 64-bit MSVC.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT TARGET lwallpaper_dependencies)
    include(cmake/Dependencies.cmake)
endif()
add_library(livewallpaper STATIC src/wallpaper.cpp)
add_library(livewallpaper::livewallpaper ALIAS livewallpaper)
set_target_properties(livewallpaper PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(livewallpaper
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE src)
target_compile_definitions(livewallpaper PRIVATE UNICODE _UNICODE DEBUG_ENABLED=0)
target_compile_options(livewallpaper PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(livewallpaper PRIVATE lwallpaper_dependencies
    d3d11 dxgi d3dcompiler dwmapi shcore winmm)

option(LIVEWALLPAPER_BUILD_EXAMPLES "Build the console example and manual desktop selftest" ${PROJECT_IS_TOP_LEVEL})
if(LIVEWALLPAPER_BUILD_EXAMPLES)
    add_executable(livewallpaper_example examples/play.cpp)
    add_executable(livewallpaper_selftest tests/selftest.cpp)
    foreach(target livewallpaper_example livewallpaper_selftest)
        target_link_libraries(${target} PRIVATE livewallpaper)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endforeach()
endif()

include(CTest)
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tests)
endif()
# Runtime files belong beside the consumer executable, not in the static lib.
if(PROJECT_IS_TOP_LEVEL)
    foreach(target livewallpaper_example livewallpaper_selftest)
        if(TARGET ${target})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${LWALLPAPER_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS livewallpaper lwallpaper_dependencies EXPORT livewallpaperTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT livewallpaperTargets NAMESPACE livewallpaper::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/livewallpaper")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfig.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/livewallpaperTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/livewallpaperConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/livewallpaper")
