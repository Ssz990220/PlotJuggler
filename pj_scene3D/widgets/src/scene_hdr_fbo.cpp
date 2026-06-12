// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_hdr_fbo.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pj::scene3d {
namespace {

template <typename Callback>
decltype(auto) withGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    throw std::runtime_error("No current OpenGL context");
  }

  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    return callback(*functions);
  }

  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    throw std::runtime_error("No OpenGL functions available");
  }
  functions->initializeOpenGLFunctions();
  return callback(*functions);
}

template <typename Callback>
void withGlFunctionsNoThrow(Callback&& callback) noexcept {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return;
  }

  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    callback(*functions);
    return;
  }

  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    return;
  }
  functions->initializeOpenGLFunctions();
  callback(*functions);
}

template <typename Callback>
decltype(auto) withCoreGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    throw std::runtime_error("No current OpenGL context");
  }

  auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context);
  if (functions == nullptr) {
    throw std::runtime_error("No OpenGL 4.5 Core functions available");
  }
  functions->initializeOpenGLFunctions();
  return callback(*functions);
}

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
      msaa_color_(std::exchange(other.msaa_color_, 0U)),
      msaa_depth_(std::exchange(other.msaa_depth_, 0U)) {}

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
    msaa_color_ = std::exchange(other.msaa_color_, 0U);
    msaa_depth_ = std::exchange(other.msaa_depth_, 0U);
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
    render_fbo_.bind();
    allocateMultisampleTexture(msaa_color_, GL_COLOR_ATTACHMENT0, GL_RGBA16F, samples_, width_, height_);
    allocateMultisampleTexture(msaa_depth_, GL_DEPTH_ATTACHMENT, GL_DEPTH_COMPONENT32F, samples_, width_, height_);
    attachDrawBuffer();
    const bool render_complete = render_fbo_.checkComplete();

    resolve_fbo_.bind();
    resolve_color_.allocate(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, width_, height_);
    withGlFunctions([this](auto& functions) {
      functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_color_.id(), 0);
    });
    resolve_depth_.allocate(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, width_, height_);
    withGlFunctions([this](auto& functions) {
      functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, resolve_depth_.id(), 0);
    });
    attachDrawBuffer();
    const bool resolve_complete = resolve_fbo_.checkComplete();
    ready_ = render_complete && resolve_complete;
    return;
  }

  resolve_fbo_.bind();
  resolve_color_.allocate(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_color_.id(), 0);
  });
  resolve_depth_.allocate(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, width_, height_);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, resolve_depth_.id(), 0);
  });
  attachDrawBuffer();
  ready_ = resolve_fbo_.checkComplete();
}

void SceneHdrFbo::bind() {
  if (samples_ > 1) {
    render_fbo_.bind();
  } else {
    resolve_fbo_.bind();
  }
}

void SceneHdrFbo::resolve() {
  if (!ready_ || samples_ <= 1) {
    return;
  }

  withGlFunctions([this](auto& functions) {
    functions.glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo_.id());
    functions.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo_.id());
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    functions.glReadBuffer(GL_COLOR_ATTACHMENT0);
    // Plural form: QOpenGLExtraFunctions has no singular glDrawBuffer, and this
    // generic lambda instantiates for both function sets.
    functions.glDrawBuffers(1, &draw_buffer);
    functions.glBlitFramebuffer(
        0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  });
}

void SceneHdrFbo::releaseGL() {
  deleteMultisampleTextures();
  resolve_color_ = gl::Texture{};
  resolve_depth_ = gl::Texture{};
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

QSize SceneHdrFbo::size() const noexcept {
  return QSize(width_, height_);
}

bool SceneHdrFbo::ready() const noexcept {
  return ready_ && hasAllocatedIds();
}

bool SceneHdrFbo::hasAllocatedIds() const noexcept {
  if (resolve_fbo_.id() == 0U || resolve_color_.id() == 0U || resolve_depth_.id() == 0U) {
    return false;
  }
  if (samples_ <= 1) {
    return true;
  }
  return render_fbo_.id() != 0U && msaa_color_ != 0U && msaa_depth_ != 0U;
}

void SceneHdrFbo::deleteMultisampleTextures() noexcept {
  if (msaa_color_ == 0U && msaa_depth_ == 0U) {
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
  });
}

}  // namespace pj::scene3d
