# ---------------------------------------------------------------------------
# nanocdr — single-header CDR (Common Data Representation) encoder/decoder,
# Apache-2.0. Used by loader-side adapters that translate ROS 2 CDR messages
# into pj_scene_protocol's canonical wire format.
# ---------------------------------------------------------------------------

add_library(nanocdr INTERFACE)
target_include_directories(nanocdr INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(nanocdr INTERFACE cxx_std_20)
