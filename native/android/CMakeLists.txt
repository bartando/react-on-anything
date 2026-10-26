# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
# The Android TV host: a NativeActivity drawing with the GPU renderer through OpenGL ES 3.2. Built with
# the NDK's toolchain by tools/android.py, which packages libmain.so into an APK.
cmake_minimum_required(VERSION 3.16)
project(react-on-anything-android C CXX)
if(NOT FFMPEG_PREFIX)
  message(FATAL_ERROR "FFMPEG_PREFIX must name FFmpeg built for this ABI (tools/ffmpeg.py)")
endif()
if(NOT ANDROID_DEPS)
  message(FATAL_ERROR "ANDROID_DEPS must name the prefix holding curl and OpenSSL built for this ABI")
endif()
add_library(harfbuzz STATIC "${HB_ROOT}/src/harfbuzz.cc" "${HB_ROOT}/src/hb-raster-draw.cc" "${HB_ROOT}/src/hb-raster-image.cc")
target_compile_features(harfbuzz PRIVATE cxx_std_17)
target_compile_definitions(harfbuzz PUBLIC HB_TINY HB_HAS_RASTER)
target_compile_options(harfbuzz PRIVATE -fno-exceptions -fno-rtti -w)
target_include_directories(harfbuzz PUBLIC "${HB_ROOT}/src")
add_library(sheenbidi STATIC "${SB_ROOT}/Source/SheenBidi.c")
target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
target_compile_options(sheenbidi PRIVATE -w)
target_include_directories(sheenbidi PUBLIC "${SB_ROOT}/Headers" PRIVATE "${SB_ROOT}/Source")
# Same engine options as the PS5 and the desktop preview.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ps5" runtime)
include("${CMAKE_CURRENT_SOURCE_DIR}/../shared/core_sources.cmake")
add_library(android_native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(android_native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

set(SHARED ../shared)
add_library(main SHARED ${ROA_CORE_SOURCES} host.cpp host_platform.cpp network_platform.cpp java.cpp media_codec.cpp gl_video.cpp image_decoder.cpp
  ${SHARED}/archives_unsupported.cpp ${SHARED}/packages_unsupported.cpp ${SHARED}/browser_capture_unsupported.cpp ${SHARED}/host_posix.cpp ${SHARED}/host_defaults.cpp
  ${SHARED}/video_player.cpp ${SHARED}/video_subtitles.cpp ${SHARED}/annex_b.cpp "${HUI_ROOT}/src/audio/mixer.cpp"
  "${APP_LAYOUT}/assets.generated.c" "${APP_LAYOUT}/sounds.generated.c")
target_compile_features(main PRIVATE cxx_std_20)
target_compile_options(main PRIVATE -Wall -Wextra -Werror)
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SHARED} "${APP_GENERATED}" "${STB_IMAGE_DIR}" "${HUI_ROOT}/src"
  "${ER_ROOT}/engine/text" "${FFMPEG_PREFIX}/include" "${ANDROID_DEPS}/include")
# Downloads past 2 GiB need 64-bit file offsets on 32-bit ARM too.
target_compile_definitions(main PRIVATE _FILE_OFFSET_BITS=64 ROA_WIDTH=${APP_LAYOUT_WIDTH} ROA_HEIGHT=${APP_LAYOUT_HEIGHT})
# NativeActivity finds ANativeActivity_onCreate in the glue, which nothing else references.
target_link_options(main PRIVATE "-u" "ANativeActivity_onCreate")
target_link_libraries(main PRIVATE android_native_app_glue er-bridge-quickjs er-software harfbuzz sheenbidi
  "${FFMPEG_PREFIX}/lib/libavformat.a" "${FFMPEG_PREFIX}/lib/libavcodec.a" "${FFMPEG_PREFIX}/lib/libswscale.a"
  "${FFMPEG_PREFIX}/lib/libswresample.a" "${FFMPEG_PREFIX}/lib/libavutil.a"
  "${ANDROID_DEPS}/lib/libcurl.a" "${ANDROID_DEPS}/lib/libssl.a" "${ANDROID_DEPS}/lib/libcrypto.a"
  android log EGL GLESv3 aaudio mediandk jnigraphics z)
