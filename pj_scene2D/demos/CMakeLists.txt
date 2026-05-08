find_package(mcap REQUIRED)
find_package(libjpeg-turbo REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjScene2DFfmpeg.cmake")
pj_scene2d_find_ffmpeg(PJ_SCENE2D_HAS_FFMPEG PJ_SCENE2D_FFMPEG_TARGETS PJ_SCENE2D_FFMPEG_SOURCE)

# --- CLI tool: extract a frame to PPM ---
add_executable(extract_frame extract_frame.cpp)
target_compile_features(extract_frame PRIVATE cxx_std_20)
target_compile_options(extract_frame PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(extract_frame PRIVATE
  pj_scene2d_core pj_datastore mcap::mcap libjpeg-turbo::libjpeg-turbo
)

# --- Qt demo: MCAP image viewer with ObjectStore pipeline ---
# Uses QRhiWidget (pj_scene2d_widgets) if Qt 6.8+ is available, else QLabel fallback
if(TARGET pj_scene2d_widgets)
  set(CMAKE_AUTOMOC ON)
  find_package(zstd REQUIRED)
  add_executable(mcap_image_viewer
    mcap_image_viewer.cpp
    cdr_detection2d_to_image_annotation.cpp
    cdr_yolo_to_image_annotation.cpp
    marker_palette.cpp
  )
  target_compile_features(mcap_image_viewer PRIVATE cxx_std_20)
  target_compile_options(mcap_image_viewer PRIVATE ${PJ_WARNING_FLAGS})
  target_compile_definitions(mcap_image_viewer PRIVATE PJ_HAS_RHI_WIDGET)
  target_link_libraries(mcap_image_viewer PRIVATE
    pj_scene2d_widgets pj_scene2d_core pj_scene_protocol pj_datastore mcap::mcap
    libjpeg-turbo::libjpeg-turbo zstd::libzstd_static nanocdr
  )

  # --- Qt demo: kPoints quad overlay visual sanity check ---
  add_executable(quad_overlay_demo quad_overlay_demo.cpp)
  target_compile_features(quad_overlay_demo PRIVATE cxx_std_20)
  target_compile_options(quad_overlay_demo PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(quad_overlay_demo PRIVATE
    pj_scene2d_widgets pj_scene2d_core pj_datastore libjpeg-turbo::libjpeg-turbo
  )

  # Tests for the loader-side CDR adapters. Shares sources with the demo so
  # there's only one place to fix bugs.
  if(PJ_BUILD_TESTS)
    add_executable(cdr_to_image_annotation_test
      cdr_to_image_annotation_test.cpp
      cdr_detection2d_to_image_annotation.cpp
      cdr_yolo_to_image_annotation.cpp
      marker_palette.cpp
    )
    target_compile_features(cdr_to_image_annotation_test PRIVATE cxx_std_20)
    target_compile_options(cdr_to_image_annotation_test PRIVATE
      ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
    )
    target_link_libraries(cdr_to_image_annotation_test PRIVATE
      pj_scene_protocol pj_base GTest::gtest_main nanocdr
    )
    add_test(NAME cdr_to_image_annotation_test COMMAND cdr_to_image_annotation_test)
  endif()

  # --- Qt demo: multi-channel viewer (color + depth side by side) ---
  add_executable(multi_channel_viewer multi_channel_viewer.cpp)
  target_compile_features(multi_channel_viewer PRIVATE cxx_std_20)
  target_compile_options(multi_channel_viewer PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(multi_channel_viewer PRIVATE
    pj_scene2d_widgets pj_scene2d_core pj_datastore mcap::mcap libjpeg-turbo::libjpeg-turbo
  )

  # --- Qt demo: simulated live stream (M13) ---
  add_executable(simulated_stream simulated_stream.cpp)
  target_compile_features(simulated_stream PRIVATE cxx_std_20)
  target_compile_options(simulated_stream PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(simulated_stream PRIVATE
    pj_scene2d_widgets pj_scene2d_core pj_datastore libjpeg-turbo::libjpeg-turbo
  )

  # --- Qt demo: video stream (M18 — FFmpeg decode via ObjectStore) ---
  if(PJ_SCENE2D_HAS_FFMPEG)
    add_executable(video_stream_demo video_stream_demo.cpp)
    target_compile_features(video_stream_demo PRIVATE cxx_std_20)
    target_compile_options(video_stream_demo PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(video_stream_demo PRIVATE
      pj_scene2d_widgets pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
    )
  endif()

  # --- Qt demo: MP4 video player (FileVideoSource) ---
  if(PJ_SCENE2D_HAS_FFMPEG)
    add_executable(mp4_video_viewer mp4_video_viewer.cpp)
    target_compile_features(mp4_video_viewer PRIVATE cxx_std_20)
    target_compile_options(mp4_video_viewer PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(mp4_video_viewer PRIVATE pj_scene2d_widgets pj_scene2d_core)
  endif()
endif()
