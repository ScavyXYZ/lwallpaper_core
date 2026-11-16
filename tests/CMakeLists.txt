add_executable(lwallpaper_performance_tests performance_tests.cpp)
target_include_directories(lwallpaper_performance_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(lwallpaper_performance_tests PRIVATE UNICODE _UNICODE DEBUG_ENABLED=0)
target_compile_options(lwallpaper_performance_tests PRIVATE /W4 /utf-8)
target_link_libraries(lwallpaper_performance_tests PRIVATE lwallpaper_dependencies
    d3d11 dxgi d3dcompiler dwmapi shcore winmm)
set_target_properties(lwallpaper_performance_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_executable(livewallpaper_api_tests api_tests.cpp)
target_link_libraries(livewallpaper_api_tests PRIVATE livewallpaper)
set_target_properties(livewallpaper_api_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(target lwallpaper_performance_tests livewallpaper_api_tests)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${LWALLPAPER_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endforeach()
add_test(NAME performance_unit COMMAND lwallpaper_performance_tests --unit)
add_test(NAME public_api COMMAND livewallpaper_api_tests)
set_tests_properties(performance_unit public_api PROPERTIES TIMEOUT 15)
find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
if(FFMPEG_EXECUTABLE AND POWERSHELL_EXECUTABLE)
    set(_media "${CMAKE_BINARY_DIR}/test-data/$<CONFIG>")
    add_test(NAME prepare_media COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/prepare_media.ps1"
        -FFmpeg "${FFMPEG_EXECUTABLE}" -Directory "${_media}")
    set_tests_properties(prepare_media PROPERTIES FIXTURES_SETUP media TIMEOUT 60)
    add_test(NAME import_and_decoder COMMAND lwallpaper_performance_tests --media
        "${_media}/h264.mp4" "${_media}/ffv1.mkv" "${_media}")
    add_test(NAME d3d11_gpu_smoke COMMAND lwallpaper_performance_tests --gpu "${_media}/h264.mp4")
    set_tests_properties(import_and_decoder d3d11_gpu_smoke PROPERTIES FIXTURES_REQUIRED media TIMEOUT 90)
    set_tests_properties(d3d11_gpu_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()
