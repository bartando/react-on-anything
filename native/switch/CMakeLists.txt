# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
# The Nintendo Switch host: a homebrew NRO drawing with the GPU renderer through Mesa's OpenGL ES 3.2.
# Built with devkitPro's toolchain (Switch.cmake) by tools/switch.py, which packs the bundle into its
# RomFS. FFmpeg, curl and Mesa are devkitPro's switch-* packages.
cmake_minimum_required(VERSION 3.16)
project(react-on-anything-switch C CXX)
add_library(harfbuzz STATIC "${HB_ROOT}/src/harfbuzz.cc" "${HB_ROOT}/src/hb-raster-draw.cc" "${HB_ROOT}/src/hb-raster-image.cc")
target_compile_features(harfbuzz PRIVATE cxx_std_17)
target_compile_definitions(harfbuzz PUBLIC HB_TINY HB_HAS_RASTER)
target_compile_options(harfbuzz PRIVATE -fno-exceptions -fno-rtti -w)
target_include_directories(harfbuzz PUBLIC "${HB_ROOT}/src")
add_library(sheenbidi STATIC "${SB_ROOT}/Source/SheenBidi.c")
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
target_compile_options(sheenbidi PRIVATE -w)
target_include_directories(sheenbidi PUBLIC "${SB_ROOT}/Headers" PRIVATE "${SB_ROOT}/Source")
# Newlib has no dynamic loading, and so no libdl for QuickJS to link.
set(CMAKE_DL_LIBS "")
# Same engine options as the PS5 and the desktop preview.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ps5" runtime)

set(SHARED ../shared)
add_executable(main host.cpp host_platform.cpp network_platform.cpp unavailable.cpp libc_shims.cpp video_decoder.cpp
  ../desktop/package_installer.cpp ../desktop/browser_capture.cpp
  ${SHARED}/network.cpp ${SHARED}/network_api.cpp
  ${SHARED}/package_api.cpp ${SHARED}/browser_capture_api.cpp ${SHARED}/lan.cpp ${SHARED}/lan_api.cpp
  ${SHARED}/image_loader.cpp ${SHARED}/image_api.cpp ${SHARED}/sound_api.cpp ${SHARED}/audio_sink.cpp
  ${SHARED}/audio_output_api.cpp ${SHARED}/host_api.cpp ${SHARED}/removals.cpp "${HUI_ROOT}/src/audio/mixer.cpp"
  ${SHARED}/host_core.cpp ${SHARED}/gl_presenter.cpp ${SHARED}/frame_stats.cpp ${SHARED}/js_heap.cpp ${SHARED}/js_profiler.cpp
  ${SHARED}/damage_tracker.cpp ${SHARED}/canvas_trace.cpp ${SHARED}/gpu_renderer.cpp ${SHARED}/render_workers.cpp
  ${SHARED}/input_script.cpp ${SHARED}/screenshot.cpp ${SHARED}/text_shaper.cpp ${SHARED}/video_api.cpp
  ${SHARED}/video_player.cpp ${SHARED}/video_subtitles.cpp ${SHARED}/gl_video.cpp ${SHARED}/gl_upscale.cpp
  "${APP_GENERATED}/1920x1080/sounds.generated.c")
# Both layouts' baked images and fonts, each registered by its own er_register_assets_<width>x<height>: the
# host picks the layout at launch, 720p handheld and 1080p docked.
foreach(LAYOUT 1920x1080 1280x720)
  set(ASSETS "${APP_GENERATED}/${LAYOUT}/assets.generated.c")
  target_sources(main PRIVATE "${ASSETS}")
  set_source_files_properties("${ASSETS}" PROPERTIES COMPILE_DEFINITIONS "er_register_assets=er_register_assets_${LAYOUT}")
endforeach()
target_compile_features(main PRIVATE cxx_std_20)
target_compile_options(main PRIVATE -Wall -Wextra -Werror)
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SHARED} "${APP_GENERATED}" "${STB_IMAGE_DIR}" "${HUI_ROOT}/src"
  "${ER_ROOT}/engine/text" "${DEVKITPRO}/portlibs/switch/include")
target_link_directories(main PRIVATE "${DEVKITPRO}/portlibs/switch/lib")
target_compile_definitions(main PRIVATE ROA_APP_NAME="${APP_NAME}")
target_link_libraries(main PRIVATE er-bridge-quickjs er-software harfbuzz sheenbidi
  avformat avcodec swscale swresample avutil dav1d bz2 curl z EGL GLESv2 glapi drm_nouveau)

nx_generate_nacp(main.nacp NAME "${APP_TITLE}" AUTHOR "React on Anything" VERSION "${APP_VERSION}")
nx_create_nro(main OUTPUT "${NRO_OUTPUT}" NACP main.nacp ICON "${APP_ICON}" ROMFS "${APP_ROMFS}")
