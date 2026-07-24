# SPDX-License-Identifier: MPL-2.0
# Package test: consume the ffmpeg package exactly the way PJ4 does
# (pj_scene2D/cmake/PjScene2DFfmpeg.cmake's pkg-config fallback), then
# compile, LINK, and run — this is what catches an import-lib/.pc layout
# mismatch that `pkg-config --exists` alone would miss.
cmake_minimum_required(VERSION 3.20)
project(pj_ffmpeg_consumer C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(LIBSWSCALE REQUIRED IMPORTED_TARGET libswscale)

add_executable(consumer main.c)
target_link_libraries(consumer
  PkgConfig::LIBAVCODEC
  PkgConfig::LIBAVFORMAT
  PkgConfig::LIBAVUTIL
  PkgConfig::LIBSWSCALE
)
