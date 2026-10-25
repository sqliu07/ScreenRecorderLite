cmake_minimum_required(VERSION 3.25)

project(ScreenRecorderLite VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "ScreenRecorderLite targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Portable build: static MSVC runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(SRL_BUILD_PROBE "Build the M0 technical-feasibility probe (development tool)" ON)
option(SRL_BUILD_TESTS "Build unit tests" ON)

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /external:anglebrackets
        /external:W0
    )
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        add_compile_options(/MP)
    endif()
endif()

# ---------------------------------------------------------------------------
# srl_logic: pure, platform-independent rules (unit tested).
# ---------------------------------------------------------------------------
add_library(srl_logic STATIC
    src/core/bgra_to_nv12.cpp
    src/core/output_size.cpp
    src/core/storage.cpp
)
target_include_directories(srl_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(srl_logic PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# srl_core: Win32 / COM / Media Foundation diagnostic building blocks.
# ---------------------------------------------------------------------------
add_library(srl_core STATIC
    src/common/win32.cpp
    src/platform/system_info.cpp
    src/platform/displays.cpp
    src/audio/audio_probe.cpp
    src/media/mf_util.cpp
    src/media/codec_probe.cpp
    src/media/mux_probe.cpp
    src/media/mp4_muxer.cpp
    src/image/png_probe.cpp
    src/image/png_writer.cpp
    src/capture/wgc_probe.cpp
    src/capture/exclusion_probe.cpp
    src/recording/audio_capture.cpp
    src/recording/screen_capture.cpp
    src/recording/recorder.cpp
)
target_link_libraries(srl_core
    PUBLIC
        srl_logic
        windowsapp
        d3d11
        dxgi
        mfplat
        mfreadwrite
        mfuuid
        ole32
        oleaut32
        shell32
        shcore
        strmiids
        uuid
        rpcrt4
        shlwapi
        user32
        gdi32
        advapi32
        propsys
        windowscodecs
        wtsapi32
        dwmapi
)
target_compile_features(srl_core PUBLIC cxx_std_20)

# Embed the application manifest through an .rc file so the Common Controls v6
# dependency and Per-Monitor V2 awareness are present in every executable.
set(SRL_MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
file(TO_CMAKE_PATH "${SRL_MANIFEST_FILE}" SRL_MANIFEST_FILE_CMAKE)
set(SRL_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
file(TO_CMAKE_PATH "${SRL_ICON_FILE}" SRL_ICON_FILE_CMAKE)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" @ONLY)

function(srl_attach_manifest target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")
    if(MSVC)
        # The manifest is embedded by RC; disable the linker-generated one to
        # avoid a duplicate RT_MANIFEST resource.
        target_link_options(${target} PRIVATE "/MANIFEST:NO")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Development probe executable. Not part of the portable release package.
# ---------------------------------------------------------------------------
if(SRL_BUILD_PROBE)
    add_executable(srl_probe
        src/probe/main.cpp
        src/probe/test_pattern.cpp
        src/probe/test_tone.cpp
    )
    target_link_libraries(srl_probe PRIVATE srl_core winmm)
    target_compile_definitions(srl_probe PRIVATE SRL_VERSION="${PROJECT_VERSION}")
    srl_attach_manifest(srl_probe)
endif()

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
add_executable(ScreenRecorderLite WIN32
    src/app/main.cpp
    src/app/application.cpp
    src/app/region_selector.cpp
    src/app/floating_control.cpp
    src/app/ui_text.cpp
    src/app/ui_draw.cpp
    src/app/custom_size_dialog.cpp
    src/app/screenshot.cpp
    src/app/settings.cpp
)
target_link_libraries(ScreenRecorderLite PRIVATE srl_core comctl32 gdiplus uxtheme)
target_compile_definitions(ScreenRecorderLite PRIVATE SRL_VERSION="${PROJECT_VERSION}")
srl_attach_manifest(ScreenRecorderLite)

set(SRL_FFMPEG_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
set(SRL_FFMPEG_EXE "" CACHE FILEPATH "FFmpeg executable to bundle (defaults to vcpkg)")
if(NOT SRL_FFMPEG_EXE)
    set(SRL_FFMPEG_EXE "${SRL_FFMPEG_ROOT}/tools/ffmpeg/ffmpeg.exe")
endif()
if(NOT EXISTS "${SRL_FFMPEG_EXE}")
    message(FATAL_ERROR "vcpkg FFmpeg executable is missing: ${SRL_FFMPEG_EXE}")
endif()
file(GLOB SRL_FFMPEG_DLLS CONFIGURE_DEPENDS "${SRL_FFMPEG_ROOT}/bin/*.dll")
add_custom_command(TARGET ScreenRecorderLite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRL_FFMPEG_EXE}"
            "$<TARGET_FILE_DIR:ScreenRecorderLite>/ffmpeg.exe"
    COMMAND_EXPAND_LISTS
    VERBATIM
)
foreach(SRL_FFMPEG_DLL IN LISTS SRL_FFMPEG_DLLS)
    add_custom_command(TARGET ScreenRecorderLite POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRL_FFMPEG_DLL}"
                "$<TARGET_FILE_DIR:ScreenRecorderLite>"
        VERBATIM
    )
endforeach()
if(TARGET srl_probe)
    add_custom_command(TARGET srl_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRL_FFMPEG_EXE}"
                "$<TARGET_FILE_DIR:srl_probe>/ffmpeg.exe"
        VERBATIM
    )
    foreach(SRL_FFMPEG_DLL IN LISTS SRL_FFMPEG_DLLS)
        add_custom_command(TARGET srl_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRL_FFMPEG_DLL}"
                    "$<TARGET_FILE_DIR:srl_probe>"
            VERBATIM
        )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SRL_BUILD_TESTS)
    enable_testing()
    add_executable(srl_core_tests tests/core_tests.cpp)
    target_link_libraries(srl_core_tests PRIVATE srl_logic)
    add_test(NAME core_rules COMMAND srl_core_tests)

    add_executable(srl_image_tests tests/image_tests.cpp)
    target_link_libraries(srl_image_tests PRIVATE srl_core)
    add_test(NAME image_file_safety COMMAND srl_image_tests)

    add_executable(srl_audio_rules_tests tests/audio_rules_tests.cpp)
    target_link_libraries(srl_audio_rules_tests PRIVATE srl_logic)
    add_test(NAME audio_rules COMMAND srl_audio_rules_tests)

    add_executable(srl_recorder_storage_tests tests/recorder_storage_tests.cpp)
    target_link_libraries(srl_recorder_storage_tests PRIVATE srl_core)
    add_test(NAME recorder_storage COMMAND srl_recorder_storage_tests)

    add_executable(srl_live_recording_tests tests/live_recording_tests.cpp)
    target_link_libraries(srl_live_recording_tests PRIVATE srl_core)
    add_test(NAME live_recording_controls COMMAND srl_live_recording_tests)

    add_executable(srl_screenshot_worker_tests
        tests/screenshot_worker_tests.cpp src/app/screenshot.cpp)
    target_link_libraries(srl_screenshot_worker_tests PRIVATE srl_core)
    add_test(NAME screenshot_worker COMMAND srl_screenshot_worker_tests)
endif()
