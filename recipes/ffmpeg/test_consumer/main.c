// SPDX-License-Identifier: MPL-2.0
// Package-test consumer: links all four FFmpeg libraries the way PJ4 does and
// asserts (a) the build is LGPL-only and (b) the platform's hardware-decode
// backend is compiled in for the codecs PJ4 ships (GPU-independent check via
// avcodec_get_hw_config — no device is opened, so it runs on headless CI).
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>

// Look the decoder up BY NAME: the hwaccel configs live on the native
// decoders ("av1" is a hwaccel-only stub), while avcodec_find_decoder(id)
// prefers software decoders (AV1 resolves to libdav1d, which has none).
static int hasHwBackend(const char* decoder_name, enum AVHWDeviceType device_type) {
  const AVCodec* decoder = avcodec_find_decoder_by_name(decoder_name);
  if (!decoder) {
    return 0;
  }
  for (int index = 0;; index++) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
    if (!config) {
      return 0;
    }
    if (config->device_type == device_type) {
      return 1;
    }
  }
}

static int checkHw(const char* decoder_name, enum AVHWDeviceType device_type) {
  if (!hasHwBackend(decoder_name, device_type)) {
    fprintf(stderr, "FAIL: %s has no %s hwaccel in this build\n", decoder_name, av_hwdevice_get_type_name(device_type));
    return 1;
  }
  return 0;
}

int main(void) {
  // Touch every linked library so the linker cannot drop one silently.
  printf(
      "avcodec %u avformat %u avutil %u swscale %u\n", avcodec_version(), avformat_version(), avutil_version(),
      swscale_version());

  const char* license_text = avcodec_license();
  const char* configuration = avcodec_configuration();
  printf("license: %s\n", license_text);
  if (!strstr(license_text, "LGPL")) {
    fprintf(stderr, "FAIL: avcodec_license() does not report LGPL\n");
    return 1;
  }
  if (strstr(configuration, "--enable-gpl") || strstr(configuration, "--enable-nonfree")) {
    fprintf(stderr, "FAIL: GPL/nonfree flags present in configuration\n");
    return 2;
  }

  int failures = 0;
#ifdef _WIN32
  // D3D11VA is the backend PJ4 prefers on Windows; these fail if the CI
  // runner's Windows SDK stops exposing the required picture-parameter types.
  failures += checkHw("h264", AV_HWDEVICE_TYPE_D3D11VA);
  failures += checkHw("hevc", AV_HWDEVICE_TYPE_D3D11VA);
  failures += checkHw("av1", AV_HWDEVICE_TYPE_D3D11VA);
#else
  failures += checkHw("h264", AV_HWDEVICE_TYPE_VAAPI);
  failures += checkHw("hevc", AV_HWDEVICE_TYPE_VAAPI);
  failures += checkHw("av1", AV_HWDEVICE_TYPE_VAAPI);
#endif
  if (failures != 0) {
    return 3;
  }
  printf("consumer test: OK\n");
  return 0;
}
