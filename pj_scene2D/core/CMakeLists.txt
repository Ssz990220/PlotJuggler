# ---------------------------------------------------------------------------
# pj_scene2d_core — media decode/playback core, no Qt dependency
# ---------------------------------------------------------------------------

find_package(libjpeg-turbo REQUIRED)
find_package(PNG REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjScene2DFfmpeg.cmake")
pj_scene2d_find_ffmpeg(PJ_SCENE2D_HAS_FFMPEG PJ_SCENE2D_FFMPEG_TARGETS PJ_SCENE2D_FFMPEG_SOURCE)

set(PJ_SCENE2D_CORE_SOURCES
  src/image_decoder.cpp
  src/codec_pipeline.cpp
  src/codecs.cpp
  src/image_pipeline_source.cpp
  src/scene_pipeline_source.cpp
  src/composite_media_source.cpp
)

if(PJ_SCENE2D_HAS_FFMPEG)
  list(APPEND PJ_SCENE2D_CORE_SOURCES
    src/ffmpeg_decoder.cpp
    src/ffmpeg_backend.cpp
    src/thumbnail_cache.cpp
    src/h264_utils.cpp
    src/streaming_video_decoder.cpp
    src/file_video_source.cpp
    src/streaming_video_source.cpp
  )
endif()

add_library(pj_scene2d_core STATIC ${PJ_SCENE2D_CORE_SOURCES})
target_include_directories(pj_scene2d_core PUBLIC include)
target_compile_features(pj_scene2d_core PUBLIC cxx_std_20)
target_compile_options(pj_scene2d_core PRIVATE
  ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
)
if(MSVC)
  # libpng uses setjmp/longjmp for error handling, which MSVC warns
  # about via C4611 (non-portable interaction with C++ destruction).
  # The decoder paths that call setjmp here do not construct RAII
  # objects in the longjmp scope, so the warning is safe to silence.
  target_compile_options(pj_scene2d_core PRIVATE /wd4611)
endif()
target_link_libraries(pj_scene2d_core
  PUBLIC pj_base pj_datastore pj_plugin_sdk
  PRIVATE libjpeg-turbo::libjpeg-turbo PNG::PNG fmt::fmt
)

if(PJ_SCENE2D_HAS_FFMPEG)
  target_link_libraries(pj_scene2d_core PRIVATE ${PJ_SCENE2D_FFMPEG_TARGETS})
  target_compile_definitions(pj_scene2d_core PUBLIC PJ_HAS_FFMPEG)
  message(STATUS "pj_scene2d_core: FFmpeg found via ${PJ_SCENE2D_FFMPEG_SOURCE} -> building FfmpegDecoder")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  find_package(mcap REQUIRED)
  find_package(nlohmann_json REQUIRED)

  set(PJ_SCENE2D_CORE_TESTS
    ../tests/decoded_frame_test.cpp
    ../tests/cancel_token_test.cpp
    ../tests/image_decoder_test.cpp
    ../tests/codecs_test.cpp
    ../tests/image_pipeline_source_test.cpp
    ../tests/scene_frame_test.cpp
    ../tests/scene_pipeline_source_test.cpp
    ../tests/composite_media_source_test.cpp
  )

  foreach(test_src ${PJ_SCENE2D_CORE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE pj_scene2d_core libjpeg-turbo::libjpeg-turbo PNG::PNG GTest::gtest_main)
    target_compile_options(${test_name} PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    if(MSVC)
      # Some test helpers call libpng's setjmp(png_jmpbuf(...)) with a
      # PngWriteCtx on the stack. Same C4611 rationale as the library
      # target above; suppress here for consistency across the module.
      target_compile_options(${test_name} PRIVATE /wd4611)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  # MCAP integration test (needs mcap + nlohmann_json + ObjectStore)
  add_executable(mcap_integration_test ../tests/mcap_integration_test.cpp)
  target_link_libraries(mcap_integration_test PRIVATE
    pj_scene2d_core pj_datastore mcap::mcap nlohmann_json::nlohmann_json
    libjpeg-turbo::libjpeg-turbo GTest::gtest_main
  )
  target_compile_options(mcap_integration_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME mcap_integration_test COMMAND mcap_integration_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )

  # Streaming test: push + retention + concurrent read (M13)
  add_executable(streaming_test ../tests/streaming_test.cpp)
  target_link_libraries(streaming_test PRIVATE pj_scene2d_core pj_datastore GTest::gtest_main)
  target_compile_options(streaming_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME streaming_test COMMAND streaming_test)

  # FFmpeg decoder test (needs test video file)
  if(PJ_SCENE2D_HAS_FFMPEG)
    add_executable(ffmpeg_decoder_test ../tests/ffmpeg_decoder_test.cpp)
    target_link_libraries(ffmpeg_decoder_test PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      GTest::gtest_main
    )
    target_compile_options(ffmpeg_decoder_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME ffmpeg_decoder_test COMMAND ffmpeg_decoder_test
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(ffmpeg_decoder_test PROPERTIES
      ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../tests/lsan_suppressions.txt"
    )

    # ThumbnailCache test (thread safety, reopen, invalid files)
    add_executable(thumbnail_cache_test ../tests/thumbnail_cache_test.cpp)
    target_link_libraries(thumbnail_cache_test PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      libjpeg-turbo::libjpeg-turbo
      GTest::gtest_main
    )
    target_compile_options(thumbnail_cache_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME thumbnail_cache_test COMMAND thumbnail_cache_test
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # H.264 NAL utilities test
    add_executable(h264_utils_test ../tests/h264_utils_test.cpp)
    target_link_libraries(h264_utils_test PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      GTest::gtest_main
    )
    target_compile_options(h264_utils_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME h264_utils_test COMMAND h264_utils_test
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # StreamingVideoDecoder test (ObjectStore + FfmpegDecoder)
    add_executable(streaming_video_decoder_test ../tests/streaming_video_decoder_test.cpp)
    target_link_libraries(streaming_video_decoder_test PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      GTest::gtest_main
    )
    target_compile_options(streaming_video_decoder_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME streaming_video_decoder_test COMMAND streaming_video_decoder_test
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # StreamingVideoSource test (MediaSource adapter for StreamingVideoDecoder)
    add_executable(streaming_video_source_test2 ../tests/streaming_video_source_test.cpp)
    target_link_libraries(streaming_video_source_test2 PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      GTest::gtest_main
    )
    target_compile_options(streaming_video_source_test2 PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME streaming_video_source_test2 COMMAND streaming_video_source_test2
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # FileVideoSource test (MediaSource adapter for FfmpegBackend)
    add_executable(file_video_source_test ../tests/file_video_source_test.cpp)
    target_link_libraries(file_video_source_test PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      GTest::gtest_main
    )
    target_compile_options(file_video_source_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME file_video_source_test COMMAND file_video_source_test
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # FfmpegBackend integration tests (play, scrub, seek)
    add_executable(ffmpeg_backend_test ../tests/ffmpeg_backend_test.cpp)
    target_link_libraries(ffmpeg_backend_test PRIVATE
      pj_scene2d_core pj_datastore
      ${PJ_SCENE2D_FFMPEG_TARGETS}
      GTest::gtest_main
    )
    target_compile_options(ffmpeg_backend_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    add_test(NAME ffmpeg_backend_test COMMAND ffmpeg_backend_test
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    if(WIN32 AND PJ_SCENE2D_FFMPEG_BIN_DIRS)
      # Windows is a non-blocking portability tracker (PJ4 v1 is Linux-only). Any
      # exe that transitively links pj_scene2d_core (the FFmpeg tests AND pj_app)
      # needs FFmpeg's shared DLLs (avcodec/avformat/avutil/swscale) resolvable at
      # launch, else STATUS_DLL_NOT_FOUND (0xc0000135). Conan 2's CMakeDeps declares
      # ffmpeg::* as INTERFACE IMPORTED with no IMPORTED_LOCATION, so
      # $<TARGET_RUNTIME_DLLS:tgt> is empty. Rather than copy DLLs next to every
      # target (a fragile per-target list that silently drops new consumers), export
      # the Conan bin dir(s) so the CI Test step can prepend them to PATH for all
      # test exes at once. Proper bundling (windeployqt + CPack) is deferred until
      # Windows is a supported release target — see .github/workflows/windows-ci.yml
      # and the "Non-goals" section of the root CLAUDE.md.
      string(REPLACE ";" "\n" _pj_ffmpeg_dirs_text "${PJ_SCENE2D_FFMPEG_BIN_DIRS}")
      file(WRITE "${CMAKE_BINARY_DIR}/pj_ffmpeg_bin_dirs.txt" "${_pj_ffmpeg_dirs_text}\n")
    endif()
  endif()

  # Dual-store test: ObjectStore + DataEngine from same MCAP (M11)
  add_executable(dual_store_test ../tests/dual_store_test.cpp)
  target_link_libraries(dual_store_test PRIVATE
    pj_scene2d_core pj_datastore mcap::mcap GTest::gtest_main
  )
  target_compile_options(dual_store_test PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  add_test(NAME dual_store_test COMMAND dual_store_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
endif()
