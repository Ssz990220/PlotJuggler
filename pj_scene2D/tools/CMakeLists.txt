# pj_scene2D developer tools — opt-in, NOT built by default (gated by
# PJ_BUILD_TOOLS in the root CMakeLists.txt). These are standalone inspection
# utilities used during development; they are not part of the app build or CI.

find_package(mcap REQUIRED)
find_package(libjpeg-turbo REQUIRED)

# --- CLI tool: extract a frame from an MCAP to PPM ---
# No Qt dependency; links only the core decoder + datastore + mcap + jpeg.
add_executable(extract_frame extract_frame.cpp)
target_compile_features(extract_frame PRIVATE cxx_std_20)
target_compile_options(extract_frame PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(extract_frame PRIVATE
  pj_scene2d_core pj_datastore mcap::mcap libjpeg-turbo::libjpeg-turbo
)

# mp4_to_mcap/ is a standalone Python script (MP4 -> MCAP converter); it has no
# build target.
