// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_hdr_fbo.h"

#include <algorithm>
#include <utility>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

void attachDrawBuffer() {
  withGlFunctions([](auto& functions) {
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    functions.glDrawBuffers(1, &draw_buffer);
    functions.glReadBuffer(GL_COLOR_ATTACHMENT0);
  });
}

void allocateMultisampleTexture(
    GLuint& id, GLenum attachment, GLenum internal_format, int samples, int width, int height) {
  withCoreGlFunctions([&](auto& functions) {
    if (id == 0U) {
      functions.glGenTextures(1, &id);
    }
    functions.glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, id);
    functions.glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internal_format, width, height, GL_TRUE);
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D_MULTISAMPLE, id, 0);
  });
}

}  // namespace

SceneHdrFbo::SceneHdrFbo() = default;

SceneHdrFbo::~SceneHdrFbo() {
  releaseGL();
}

SceneHdrFbo::SceneHdrFbo(SceneHdrFbo&& other) noexcept
    : samples_(std::exchange(other.samples_, 0)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      ready_(std::exchange(other.ready_, false)),
      render_fbo_(std::move(other.render_fbo_)),
      resolve_fbo_(std::move(other.resolve_fbo_)),
      resolve_color_(std::move(other.resolve_color_)),
      resolve_depth_(std::move(other.resolve_depth_)),
      resolve_mask_(std::move(other.resolve_mask_)),
      msaa_color_(std::exchange(other.msaa_color_, 0U)),
      msaa_depth_(std::exchange(other.msaa_depth_, 0U)),
      msaa_mask_(std::exchange(other.msaa_mask_, 0U)) {}

SceneHdrFbo& SceneHdrFbo::operator=(SceneHdrFbo&& other) noexcept {
  if (this != &other) {
    releaseGL();
    samples_ = std::exchange(other.samples_, 0);
    width_ = std::exchange(other.width_, 0);
    height_ = std::exchange(other.height_, 0);
    ready_ = std::exchange(other.ready_, false);
    render_fbo_ = std::move(other.render_fbo_);
    resolve_fbo_ = std::move(other.resolve_fbo_);
    resolve_color_ = std::move(other.resolve_color_);
    resolve_depth_ = std::move(other.resolve_depth_);
    resolve_mask_ = std::move(other.resolve_mask_);
    msaa_color_ = std::exchange(other.msaa_color_, 0U);
    msaa_depth_ = std::exchange(other.msaa_depth_, 0U);
    msaa_mask_ = std::exchange(other.msaa_mask_, 0U);
  }
  return *this;
}

void SceneHdrFbo::configure(int samples) {
  const int clamped_samples = samples > 1 ? samples : 0;
  if (samples_ == clamped_samples) {
    return;
  }
  releaseGL();
  samples_ = clamped_samples;
}

void SceneHdrFbo::resize(int device_w, int device_h) {
  if (device_w <= 0 || device_h <= 0) {
    releaseGL();
    return;
  }
  if (width_ == device_w && height_ == device_h && hasAllocatedIds()) {
    return;
  }

  releaseGL();
  width_ = device_w;
  height_ = device_h;

  if (samples_ > 1) {
    // The backing FBO's achieved sample count (the seed for samples_) is bounded
    // by GL_MAX_SAMPLES, but our float color/depth multisample TEXTURES are
    // bounded by the (spec-allows-lower) GL_MAX_COLOR/DEPTH_TEXTURE_SAMPLES.
    // glTexImage2DMultisample raises GL_INVALID_OPERATION above those limits and
    // leaves the chain incomplete (L.40), so clamp here. One-way clamp of the
    // member is fine: configure() re-seeds it per context.
    withCoreGlFunctions([this](auto& functions) {
      GLint max_color_samples = 0;
      GLint max_depth_samples = 0;
      functions.glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &max_color_samples);
      functions.glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &max_depth_samples);
      samples_ = std::min({samples_, max_color_samples, max_depth_samples});
    });
  }

  if (samples_ > 1) {
    render_fbo_.bind();
    allocateMultisampleTexture(msaa_color_, GL_COLOR_ATTACHMENT0, GL_RGBA16F, samples_, width_, height_);
    allocateMultisampleTexture(msaa_depth_, GL_DEPTH_ATTACHMENT, GL_DEPTH_COMPONENT32F, samples_, width_, height_);
    // R8 "is-mesh" mask alongside color/depth, same sample count. Only the mesh
    // pass enables this draw buffer, so it stays 0 outside mesh pixels.
    allocateMultisampleTexture(msaa_mask_, kMaskAttachment, GL_R8, samples_, width_, height_);
    attachDrawBuffer();
    const bool render_complete = render_fbo_.checkComplete();
    ready_ = render_complete && allocateResolveFbo();
    return;
  }

  ready_ = allocateResolveFbo();
}

bool SceneHdrFbo::allocateResolveFbo() {
  resolve_fbo_.bind();
  // LINEAR on the resolved color so a supersampled scene (render scale > 1)
  // box-averages correctly when the present pass downsamples it to device res;
  // at scale 1 the present samples texel centers 1:1, so LINEAR == NEAREST there.
  resolve_color_.allocate(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, width_, height_, GL_LINEAR);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_color_.id(), 0);
  });
  resolve_depth_.allocate(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, resolve_depth_.id(), 0);
  });
  // Single-sample R8 mask. In the samples <= 1 path this is the live render
  // target the mesh pass writes; in the MSAA path resolve() blits into it.
  resolve_mask_.allocate(GL_R8, GL_RED, GL_UNSIGNED_BYTE, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, kMaskAttachment, GL_TEXTURE_2D, resolve_mask_.id(), 0);
  });
  attachDrawBuffer();
  return resolve_fbo_.checkComplete();
}

void SceneHdrFbo::bind() {
  if (samples_ > 1) {
    render_fbo_.bind();
  } else {
    resolve_fbo_.bind();
  }
}

void SceneHdrFbo::resolve(bool resolve_mask) {
  if (!ready_ || samples_ <= 1) {
    return;
  }

  withGlFunctions([this, resolve_mask](auto& functions) {
    functions.glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo_.id());
    functions.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_.id());
    const GLenum color_buffer = GL_COLOR_ATTACHMENT0;
    functions.glReadBuffer(GL_COLOR_ATTACHMENT0);
    // Plural form: QOpenGLExtraFunctions has no singular glDrawBuffer, and this
    // generic lambda instantiates for both function sets.
    functions.glDrawBuffers(1, &color_buffer);
    functions.glBlitFramebuffer(
        0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (resolve_mask) {
      // Resolve the mask attachment in its own pass: glBlitFramebuffer resolves
      // one color attachment at a time (selected by read/draw buffer). Skipped
      // when EDL is off — nothing samples the mask, so the blit would be waste.
      const GLenum mask_buffer = kMaskAttachment;
      functions.glReadBuffer(kMaskAttachment);
      functions.glDrawBuffers(1, &mask_buffer);
      functions.glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      // Leave both FBOs reading/drawing COLOR_ATTACHMENT0 — the state the rest of
      // the frame and the next frame's geometry pass assume.
      functions.glReadBuffer(GL_COLOR_ATTACHMENT0);
      functions.glDrawBuffers(1, &color_buffer);
    }
  });
}

void SceneHdrFbo::releaseGL() {
  deleteMultisampleTextures();
  resolve_color_ = gl::Texture{};
  resolve_depth_ = gl::Texture{};
  resolve_mask_ = gl::Texture{};
  render_fbo_ = gl::Framebuffer{};
  resolve_fbo_ = gl::Framebuffer{};
  width_ = 0;
  height_ = 0;
  ready_ = false;
}

GLuint SceneHdrFbo::resolvedColorTextureId() const noexcept {
  return resolve_color_.id();
}

GLuint SceneHdrFbo::resolvedDepthTextureId() const noexcept {
  return resolve_depth_.id();
}

GLuint SceneHdrFbo::resolvedMaskTextureId() const noexcept {
  return resolve_mask_.id();
}

QSize SceneHdrFbo::size() const noexcept {
  return QSize(width_, height_);
}

bool SceneHdrFbo::ready() const noexcept {
  return ready_ && hasAllocatedIds();
}

bool SceneHdrFbo::hasAllocatedIds() const noexcept {
  if (resolve_fbo_.id() == 0U || resolve_color_.id() == 0U || resolve_depth_.id() == 0U || resolve_mask_.id() == 0U) {
    return false;
  }
  if (samples_ <= 1) {
    return true;
  }
  return render_fbo_.id() != 0U && msaa_color_ != 0U && msaa_depth_ != 0U && msaa_mask_ != 0U;
}

void SceneHdrFbo::deleteMultisampleTextures() noexcept {
  if (msaa_color_ == 0U && msaa_depth_ == 0U && msaa_mask_ == 0U) {
    return;
  }

  withGlFunctionsNoThrow([this](auto& functions) {
    if (msaa_color_ != 0U) {
      functions.glDeleteTextures(1, &msaa_color_);
      msaa_color_ = 0U;
    }
    if (msaa_depth_ != 0U) {
      functions.glDeleteTextures(1, &msaa_depth_);
      msaa_depth_ = 0U;
    }
    if (msaa_mask_ != 0U) {
      functions.glDeleteTextures(1, &msaa_mask_);
      msaa_mask_ = 0U;
    }
  });
}

}  // namespace pj::scene3d
