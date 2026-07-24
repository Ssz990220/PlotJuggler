# Platform-neutral cloudini package config for the PJ4 pixi build.
# Mirrors the Conan package: a static cloudini::cloudini carrying lz4 + zstd
# (built from the env, matching the Conan graph) plus m/pthread on unix.

if(TARGET cloudini::cloudini)
  return()
endif()

get_filename_component(_cloudini_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

find_library(_cloudini_lib NAMES cloudini_lib libcloudini_lib
  HINTS "${_cloudini_prefix}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(_cloudini_lz4 NAMES lz4 liblz4 HINTS "${_cloudini_prefix}/lib" REQUIRED)
find_library(_cloudini_zstd NAMES zstd libzstd HINTS "${_cloudini_prefix}/lib" REQUIRED)

add_library(cloudini::cloudini STATIC IMPORTED)
set_target_properties(cloudini::cloudini PROPERTIES
  IMPORTED_LOCATION "${_cloudini_lib}"
  INTERFACE_INCLUDE_DIRECTORIES "${_cloudini_prefix}/include")

set(_cloudini_deps "${_cloudini_lz4}" "${_cloudini_zstd}")
if(UNIX)
  list(APPEND _cloudini_deps m pthread)
endif()
set_property(TARGET cloudini::cloudini APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${_cloudini_deps})

set(cloudini_FOUND TRUE)
