function(pj_scene2d_find_ffmpeg out_found out_targets out_source)
  find_package(ffmpeg CONFIG QUIET)
  if(TARGET ffmpeg::avcodec AND TARGET ffmpeg::avformat AND TARGET ffmpeg::avutil AND TARGET ffmpeg::swscale)
    set(${out_found} TRUE PARENT_SCOPE)
    set(${out_targets} ffmpeg::avcodec ffmpeg::avformat ffmpeg::avutil ffmpeg::swscale PARENT_SCOPE)
    set(${out_source} "Conan" PARENT_SCOPE)
    return()
  endif()

  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBAVCODEC IMPORTED_TARGET libavcodec)
    pkg_check_modules(LIBAVFORMAT IMPORTED_TARGET libavformat)
    pkg_check_modules(LIBAVUTIL IMPORTED_TARGET libavutil)
    pkg_check_modules(LIBSWSCALE IMPORTED_TARGET libswscale)
  endif()

  if(LIBAVCODEC_FOUND AND LIBAVFORMAT_FOUND AND LIBAVUTIL_FOUND AND LIBSWSCALE_FOUND)
    set(${out_found} TRUE PARENT_SCOPE)
    set(${out_targets}
      PkgConfig::LIBAVCODEC
      PkgConfig::LIBAVFORMAT
      PkgConfig::LIBAVUTIL
      PkgConfig::LIBSWSCALE
      PARENT_SCOPE
    )
    set(${out_source} "pkg-config" PARENT_SCOPE)
    return()
  endif()

  set(${out_found} FALSE PARENT_SCOPE)
  set(${out_targets} "" PARENT_SCOPE)
  set(${out_source} "" PARENT_SCOPE)
endfunction()
