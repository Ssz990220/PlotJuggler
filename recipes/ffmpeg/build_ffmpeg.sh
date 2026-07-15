# Build script for the PJ4 LGPL-lean FFmpeg package (linux-64 + win-64).
# Runs under bash on both platforms (rattler-build `interpreter: bash`; the
# win-64 build env provides bash via m2-bash). Mirrors conanfile.txt's trimmed
# ffmpeg/* option block: decode-only, LGPL-only, --disable-autodetect.
set -euxo pipefail

# rattler-build exports SUBDIR=<conda platform subdir>, but ffmpeg's
# ffbuild/common.mak uses `ifndef SUBDIR` as its top-level include guard — an
# inherited SUBDIR skips the block defining the LINK macro, so every
# shared-lib link expands to an EMPTY recipe line and make "succeeds" without
# producing the libraries. Unset it.
unset SUBDIR

# The LGPL-critical, platform-independent option set. --disable-autodetect is
# the keystone: nothing links unless explicitly enabled below, so no system/
# sysroot library can sneak an unaudited (or GPL) dep into the build.
common_flags=(
  --disable-gpl --disable-nonfree --disable-autodetect
  --enable-shared --disable-static
  --disable-programs --disable-doc
  --enable-avcodec --enable-avformat --enable-swscale
  --disable-avdevice --disable-avfilter --disable-swresample
  --enable-zlib
  --enable-libdav1d
  --disable-encoders --disable-muxers
  --disable-decoders
  --enable-decoder=h264 --enable-decoder=hevc --enable-decoder=mjpeg
  --enable-decoder=av1 --enable-decoder=libdav1d
  --disable-demuxers
  --enable-demuxer=mov --enable-demuxer=matroska --enable-demuxer=avi
  --enable-demuxer=h264 --enable-demuxer=hevc --enable-demuxer=mjpeg
  --disable-parsers
  --enable-parser=h264 --enable-parser=hevc --enable-parser=mjpeg
  --enable-parser=av1
  --disable-bsfs
  --enable-bsf=h264_mp4toannexb --enable-bsf=hevc_mp4toannexb
  --disable-protocols
  --enable-protocol=file
)

if [[ "${target_platform:-}" == win-* ]]; then
  # MSVC build under MSYS2 bash. Path forms matter:
  #  - mixed form (C:/…, cygpath -m) for anything cl.exe/link.exe consumes
  #    and for --prefix, so the paths baked into the .pc files stay in the
  #    native form rattler-build's prefix relocation and pkg-config expect;
  #  - MSYS form (/c/…, cygpath -u) only for PKG_CONFIG_LIBDIR, which the
  #    MSYS-side configure probes read.
  library_prefix_m="$(cygpath -m "$LIBRARY_PREFIX")"
  library_prefix_u="$(cygpath -u "$LIBRARY_PREFIX")"
  build_prefix_m="$(cygpath -m "$BUILD_PREFIX")"

  # PKG_CONFIG_LIBDIR (not _PATH): ONLY the host prefix may satisfy the
  # explicit --enable-libdav1d / zlib checks — no MSYS or system packages.
  export PKG_CONFIG_PATH=
  export PKG_CONFIG_LIBDIR="$library_prefix_u/lib/pkgconfig"

  # -MD selects the dynamic CRT (conda-wide convention; --enable-shared alone
  # does NOT set it and MSVC would default to the static CRT).
  # d3d11va/dxva2 replace the Linux vaapi/libdrm pair: header-only Windows SDK
  # hwaccel backends, LGPL-clean, no host deps.
  ./configure \
    --prefix="$library_prefix_m" \
    --toolchain=msvc \
    --target-os=win64 --arch=x86_64 \
    --disable-stripping \
    --pkg-config="$build_prefix_m/Library/bin/pkg-config.exe" \
    "${common_flags[@]}" \
    --enable-d3d11va --enable-dxva2 \
    --extra-cflags="-MD -I$library_prefix_m/include" \
    --extra-ldflags="-LIBPATH:$library_prefix_m/lib"

  # Assert the dynamic CRT actually landed in the recorded flags.
  grep -q -- "-MD" ffbuild/config.mak

  # Upstream installs the MSVC import .lib files into bin/ while the generated
  # .pc files point consumers at lib/ — configure succeeds, linking fails.
  # Same config.mak fix conda-forge's ffmpeg feedstock applies.
  sed -i 's/SLIB_INSTALL_EXTRA_LIB=$(SLIBNAME_WITH_MAJOR:$(SLIBSUF)=.def)/SLIB_INSTALL_EXTRA_LIB=$(SLIBNAME:$(SLIBSUF)=.lib)/' ffbuild/config.mak
  sed -i 's/SLIB_INSTALL_EXTRA_SHLIB=$(SLIBNAME:$(SLIBSUF)=.lib)/SLIB_INSTALL_EXTRA_SHLIB=/' ffbuild/config.mak

  make -j"${CPU_COUNT:-4}"
  make install

  # Layout contract for PjScene2DFfmpeg.cmake's pkg-config fallback:
  # import libs in Library/lib, DLLs in Library/bin.
  for name in avcodec avformat avutil swscale; do
    test -f "$library_prefix_u/lib/${name}.lib"
    ls "$library_prefix_u"/bin/${name}-*.dll
    test -f "$library_prefix_u/lib/pkgconfig/lib${name}.pc"
  done

  # Upstream's MSVC pkg-config emission writes static-style metadata: the
  # translated linker flags (-LIBPATH:…) and every dependency (zlib.lib,
  # dav1d.lib, ole32.lib, …) land on the PUBLIC Libs:/Requires: lines, so a
  # consumer of the shared DLLs is told to link import libs it never needs —
  # and which its environment may not even provide (LNK1181 on zlib.lib).
  # Normalize each .pc to the canonical shared form: the public surface is
  # exactly `-L${libdir} -l<name>`; dependency metadata moves to
  # Requires.private (still traversed for --cflags), and the stale
  # Libs.private/empty lines are dropped. conda-forge patches this same
  # emission for its Windows ffmpeg builds.
  for name in avcodec avformat avutil swscale; do
    pc="$library_prefix_u/lib/pkgconfig/lib${name}.pc"
    sed -i -E \
      -e "s|^Libs:.*|Libs: -L\${libdir} -l${name}|" \
      -e '/^Libs\.private:[[:space:]]*$/d' \
      -e '/^Requires\.private:[[:space:]]*$/d' \
      -e 's|^Requires:[[:space:]]*(.+)$|Requires.private: \1|' \
      -e '/^Requires:[[:space:]]*$/d' \
      "$pc"
    echo "--- lib${name}.pc after normalization ---"
    cat "$pc"
  done
else
  # Linux: conda cross-compiler (glibc-2.28 sysroot via recipes/variants.yaml).
  # dav1d/libdrm are probed via pkg-config; point it at the host prefix.
  export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

  ./configure \
    --prefix="$PREFIX" \
    --cc="$CC" \
    "${common_flags[@]}" \
    --enable-pic \
    --enable-vaapi --enable-libdrm \
    --extra-cflags="${CFLAGS:-} -I$PREFIX/include" \
    --extra-ldflags="${LDFLAGS:-} -L$PREFIX/lib"

  make -j"${CPU_COUNT:-$(nproc)}"
  make install
fi
