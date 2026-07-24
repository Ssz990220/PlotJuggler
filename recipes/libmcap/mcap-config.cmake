# Platform-neutral mcap package config for the PJ4 pixi build.
# Header-only INTERFACE target carrying lz4 + zstd, matching the Conan package.

if(TARGET mcap::mcap)
  return()
endif()

get_filename_component(_mcap_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

find_library(_mcap_lz4 NAMES lz4 liblz4 HINTS "${_mcap_prefix}/lib" REQUIRED)
find_library(_mcap_zstd NAMES zstd libzstd HINTS "${_mcap_prefix}/lib" REQUIRED)

add_library(mcap::mcap INTERFACE IMPORTED)
set_target_properties(mcap::mcap PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_mcap_prefix}/include"
  INTERFACE_LINK_LIBRARIES "${_mcap_lz4};${_mcap_zstd}")

set(mcap_FOUND TRUE)
