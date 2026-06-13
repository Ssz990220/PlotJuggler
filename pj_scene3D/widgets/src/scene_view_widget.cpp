// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/scene_view_widget.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPalette>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <variant>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/gl/debug.h"
#include "pj_scene3d_widgets/gl/framebuffer.h"
#include "pj_scene3d_widgets/render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

namespace pj::scene3d {

namespace {

Q_LOGGING_CATEGORY(lcSceneViewWidget, "pj.scene3d.scene_view")

QSurfaceFormat make_default_format() {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  // 4x MSAA. Requested explicitly because a QOpenGLWidget renders into an FBO
  // sized by the format's concrete sample count — the default (-1, "don't
  // care") yields a single-sampled FBO. The previous native QOpenGLWindow got
  // multisampling incidentally from the platform's default visual; the FBO path
  // does not, so without this the grid/TF/pointcloud edges alias.
  fmt.setSamples(4);
  fmt.setSwapInterval(1);  // vsync — caps render at ~60Hz on standard monitors
  // Request a debug context only when GL debug output is actually wanted; a
  // DebugContext has measurable CPU overhead on some drivers (extra validation
  // layer), so it must not be on by default in release builds.
  if (gl::debugOutputRequested()) {
    fmt.setOption(QSurfaceFormat::DebugContext);
  }
  return fmt;
}

constexpr std::string_view kPresentVertSrc = R"GLSL(
#version 450 core
out vec2 v_uv;

void main() {
  float x = float(gl_VertexID == 1) * 4.0 - 1.0;
  float y = float(gl_VertexID == 2) * 4.0 - 1.0;
  gl_Position = vec4(x, y, 0.0, 1.0);
  v_uv = vec2(x, y) * 0.5 + 0.5;
}
)GLSL";

// Composite/tonemap present (Phase 0B). The scene FBO now holds LINEAR-light
// HDR; this pass tonemaps and applies the single manual sRGB encode (the
// backing FBO is not sRGB-capable; GL_FRAMEBUFFER_SRGB stays disabled).
// AgX: adapted from three.js tonemapping_pars_fragment (MIT; Filament/Sobotka
// derived). ACES: Narkowicz (CC0). sRGB OETF: IEC 61966-2-1. Full license
// texts: pj_scene3D/THIRDPARTY.md. Will become CompositePass : IPostPass when
// EDL/SSAO inputs land (plan §A.6).
constexpr std::string_view kPresentFragSrc = R"GLSL(
#version 450 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_scene;
uniform sampler2D u_depth;
uniform sampler2D u_ao;
uniform bool u_has_ao = false;
uniform sampler2D u_edl;
uniform bool u_has_edl = false;
uniform int u_tonemap_mode = 1;  // 0 None, 1 ACES, 2 AgX
uniform float u_exposure = 1.1;
uniform float u_ao_strength = 1.0;
uniform float u_saturation = 1.2;  // post-tonemap saturation boost

vec3 sRGB(vec3 c) {
  bvec3 k = lessThanEqual(c, vec3(0.0031308));
  return mix(1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(k));
}
vec3 ACES(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 agxContrast(vec3 x) {
  vec3 x2 = x * x;
  vec3 x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
// GLSL mat3 ctors are COLUMN-major; these match three.js's AgXInset/Outset
// columns exactly. (Transposing them tints greys blue: the transposed outset's
// blue row sums to ~1.22.)
const mat3 AGX_IN = mat3(0.856627, 0.137319, 0.111898, 0.0951212, 0.761242, 0.0767994, 0.0482516, 0.101439, 0.811302);
const mat3 AGX_OUT = mat3(1.127101, -0.141330, -0.141330, -0.110607, 1.157824, -0.110607, -0.016494, -0.016494, 1.251936);
const mat3 S2R = mat3(0.627404, 0.069097, 0.016392, 0.329282, 0.919540, 0.088013, 0.043314, 0.011361, 0.895595);
const mat3 R2S = mat3(1.660500, -0.124551, -0.018151, -0.587641, 1.132900, -0.100579, -0.072850, -0.008349, 1.118730);
vec3 AgX(vec3 c) {
  c = S2R * c;
  c = AGX_IN * c;
  c = max(c, vec3(1e-10));
  c = log2(c);
  c = clamp((c + 12.47393) / (4.026069 + 12.47393), 0.0, 1.0);
  c = agxContrast(c);
  c = AGX_OUT * c;
  c = pow(max(c, vec3(0.0)), vec3(2.2));
  c = R2S * c;
  return clamp(c, 0.0, 1.0);
}
void main() {
  vec4 scene = texture(u_scene, v_uv);
  float depth = texture(u_depth, v_uv).r;
  vec3 hdr = scene.rgb * u_exposure;
  if (u_has_ao) {
    hdr *= mix(1.0, texture(u_ao, v_uv).r, u_ao_strength);
  }
  if (u_has_edl) {
    hdr *= texture(u_edl, v_uv).r;  // eye-dome shade factor (1 = untouched)
  }
  vec3 graded = u_tonemap_mode == 1 ? ACES(hdr) : u_tonemap_mode == 2 ? AgX(hdr) : clamp(hdr, vec3(0.0), vec3(1.0));
  float luma = dot(graded, vec3(0.2126, 0.7152, 0.0722));
  graded = clamp(mix(vec3(luma), graded, u_saturation), vec3(0.0), vec3(1.0));
  if (depth >= 0.999999) {
    graded = clamp(scene.rgb, vec3(0.0), vec3(1.0));
  }
  // scene.a is the annotation marker (TF axes/HUD write 0): bypass the grade so
  // synthetic markers keep their flat vivid colors, while data objects (alpha 1)
  // get the filmic look. MSAA resolve averages the marker, feathering the seam.
  vec3 ldr = mix(clamp(scene.rgb, vec3(0.0), vec3(1.0)), graded, scene.a);
  frag = vec4(sRGB(ldr), 1.0);  // alpha forced 1.0: the Wayland opaque-surface invariant
}
)GLSL";

}  // namespace

SceneViewWidget::SceneViewWidget(QWidget* parent) : QOpenGLWidget(parent) {
  setFormat(make_default_format());
  setMinimumSize(320, 240);
  setMouseTracking(false);
}

SceneViewWidget::~SceneViewWidget() {
  // Disconnect the teardown hook first so aboutToBeDestroyed can't fire on this
  // half-destroyed object when the base QOpenGLWidget destroys the context, then
  // free GL resources while our members and the context are still alive.
  QObject::disconnect(context_cleanup_connection_);
  releaseGlResources();
}

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setTrackerTime(PJ::Timepoint t) {
  if (render_time_ == t) {
    return;
  }
  render_time_ = t;
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setFixedFrame(const std::string& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  update();
}

void SceneViewWidget::setAxesVisible(bool visible) {
  if (axes_visible_ == visible) {
    return;
  }
  axes_visible_ = visible;
  update();
}

void SceneViewWidget::setLayers(const std::vector<Scene3DLayer*>& ordered) {
  if (layers_ == ordered) {
    return;
  }
  if (context() != nullptr) {
    makeCurrent();
    for (Scene3DLayer* layer : layers_) {
      if (layer != nullptr && std::find(ordered.begin(), ordered.end(), layer) == ordered.end()) {
        layer->releaseGL();
      }
    }
    doneCurrent();
  }
  layers_ = ordered;
  update();
}

void SceneViewWidget::refreshAvailableFrames() {
  QList<FrameRow> list;
  if (tf_) {
    auto rows = tf_->getFrameHierarchy();
    list.reserve(static_cast<qsizetype>(rows.size()));
    for (auto&& r : rows) {
      list.append(FrameRow{std::move(r.name), r.depth});
    }
  }
  if (list != last_frame_list_) {
    last_frame_list_ = list;
    emit framesChanged(list);
  }
}

void SceneViewWidget::initializeGL() {
  // Qt calls this once per GL context — on first realize and again after every
  // context recreation (QOpenGLWidget rebuilds its context when reparented by
  // ADS dock/float/split). Rewire the teardown hook to THIS context so the
  // dying context releases its own resources; releaseGlResources() already ran
  // for the previous context (via its aboutToBeDestroyed), clearing the passes'
  // initialized_ latches, so the rebuild below starts from a clean slate.
  QObject::disconnect(context_cleanup_connection_);
  context_cleanup_connection_ = connect(
      context(), &QOpenGLContext::aboutToBeDestroyed, this, &SceneViewWidget::releaseGlResources, Qt::DirectConnection);

  gl::installDebugCallback();
  // The HDR scene FBO must match the backing FBO's ACHIEVED sample count (the
  // driver may grant fewer than the 4 samples make_default_format() requests);
  // <=1 selects the single-sample chain. Attachments are (re)allocated lazily in
  // paintGL, sized from the viewport Qt set for the backing FBO.
  const int scene_samples = std::max(context()->format().samples(), 0);
  scene_fbo_.configure(scene_samples);
  initializePresentProgram();
  scene_fbo_fallback_logged_ = false;  // a fresh context may succeed; re-arm the warning

  axes_.initializeGL();
  grid_.initializeGL();
  overlay_.initializeGL();
  // Layer GL is initialised lazily in paintGL — layers may be added
  // dynamically after the widget is already realised, so initializing
  // here would miss late entries. releaseGlResources() reset their lazy-init
  // guards, so they rebuild on the first paint after a context recreation.
}

void SceneViewWidget::initializePresentProgram() {
  auto result = gl::Program::fromSources(kPresentVertSrc, kPresentFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    present_program_.emplace(std::move(*program));
    // The sampler units are CONSTANT (u_scene=0, u_depth=1, u_ao=2, u_edl=3) — set
    // them once at build time rather than every frame in paintGL (L.57). Only the
    // texture *binds* and the value/flag uniforms change per frame.
    present_program_->use();
    present_program_->setInt("u_scene", 0);
    present_program_->setInt("u_depth", 1);
    present_program_->setInt("u_ao", 2);
    present_program_->setInt("u_edl", 3);
    if (auto* ctx = QOpenGLContext::currentContext(); ctx != nullptr) {
      if (auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx); funcs != nullptr) {
        funcs->glUseProgram(0);
      }
    }
  } else {
    qCWarning(lcSceneViewWidget) << "present shader error:" << std::get<std::string>(result).c_str();
    present_program_.reset();
  }
}

void SceneViewWidget::releaseGlResources() {
  // Drop every pass's and layer's GL objects with a context current so their
  // glDelete* actually run (the wrappers self-skip without a current context).
  // Called from the context's aboutToBeDestroyed (reparent or teardown) and the
  // destructor. context() is null before the first show / after full teardown.
  if (context() == nullptr) {
    return;
  }
  makeCurrent();
  axes_.releaseGL();
  grid_.releaseGL();
  overlay_.releaseGL();
  for (Scene3DLayer* layer : layers_) {
    if (layer != nullptr) {
      layer->releaseGL();
    }
  }
  // The HDR chain and the present program/VAO are per-context like every other
  // GL object here; zeroing them under the dying context forces a clean lazy
  // rebuild in the next context (initializeGL + first paint).
  scene_fbo_.releaseGL();
  ssao_.releaseGL();
  edl_.releaseGL();
  present_program_.reset();
  present_vao_ = gl::VertexArray{};
  doneCurrent();
}

void SceneViewWidget::resizeGL(int /*w*/, int /*h*/) {
  // Nothing to do: Qt sets the backing-FBO viewport itself, and the HDR scene
  // FBO is (re)sized lazily in paintGL from that viewport's device-pixel size.
  // (Qt 6 passes LOGICAL units here, so the viewport read is the exact source.)
}

void SceneViewWidget::paintGL() {
  auto* ctx = QOpenGLContext::currentContext();
  auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx);
  if (funcs == nullptr) {
    return;
  }

  // Device-pixel size: Qt bound the backing FBO and set the viewport to its
  // exact device size right before paintGL — reading it back is exact even at
  // fractional DPR (Qt 6 passes LOGICAL units to resizeGL, so that is not).
  GLint viewport[4] = {0, 0, 0, 0};
  funcs->glGetIntegerv(GL_VIEWPORT, viewport);
  // Per-paint locals (recomputed each frame from the viewport Qt just set);
  // there is no cross-frame device-size state worth keeping as a member.
  const int device_width_px = viewport[2];
  const int device_height_px = viewport[3];

  // (Re)allocate the off-screen HDR chain; idempotent at unchanged size. If the
  // chain or the present shader is unavailable, fall back to rendering directly
  // into the backing FBO exactly as before Phase 0A (degrade, never go blank).
  scene_fbo_.resize(device_width_px, device_height_px);
  const bool offscreen = scene_fbo_.ready() && present_program_.has_value();
  if (offscreen) {
    scene_fbo_.bind();
    funcs->glViewport(0, 0, device_width_px, device_height_px);
  } else {
    // Fallback: render directly into the backing FBO. scene_fbo_.resize() above
    // leaves an off-screen FBO bound whenever it reallocates (M.33), so we must
    // explicitly rebind the backing target here — otherwise renderScene would
    // draw into the off-screen (possibly incomplete) FBO.
    gl::Framebuffer::bindDefault(defaultFramebufferObject());
    funcs->glViewport(0, 0, device_width_px, device_height_px);
    if (!scene_fbo_fallback_logged_) {
      qCWarning(lcSceneViewWidget) << "HDR scene FBO unavailable — rendering directly into the backing framebuffer";
      scene_fbo_fallback_logged_ = true;
    }
  }

  const float aspect = static_cast<float>(width()) / static_cast<float>(std::max(height(), 1));
  // viewport_width/height_px keep their historical LOGICAL-pixel semantics: the
  // HUD overlay derives the device-pixel ratio as saved_vp[3] / viewport_height_px.
  // device_*_px carry the FRAMEBUFFER size so passes that size primitives in
  // device pixels (gl_PointSize in PointcloudRenderPass) get the HiDPI-correct
  // value instead of the logical height.
  const ViewParams view_params{
      camera_->viewMatrix(), camera_->projMatrix(aspect), height(), width(), camera_->position(), device_width_px,
      device_height_px,
      shading_params_,  // this view's mesh/collision look knobs (per-view, see header)
  };

  // Grid never consults the TF buffer; safe to render even when tf_ is null.
  static const TransformBuffer kEmptyBuffer;
  const TransformBuffer& tf_ref = tf_ ? *tf_ : kEmptyBuffer;
  // The TF-resolution triple, bundled for the passes/layers that need it.
  // fixed_frame_ is the long-lived member (no per-frame string copy).
  const FrameContext frame_ctx{tf_ref, fixed_frame_, render_time_};

  // On the direct-to-backing fallback the legacy Wayland alpha guard applies
  // (set inside renderScene, after the clear); the off-screen path instead
  // forces alpha=1.0 in the present shader.
  renderScene(view_params, frame_ctx, /*mask_alpha_writes=*/!offscreen);

  if (!offscreen) {
    // Restore the alpha write mask so the next frame's glClear repaints alpha=1.0.
    funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    return;
  }

  // Resolve MSAA -> single-sample, then present into the backing FBO. The blit
  // and the fullscreen draw must cover the full target: clear any pass-leaked
  // state (scissor/blend/depth) first rather than assuming the passes left it
  // clean.
  funcs->glDisable(GL_SCISSOR_TEST);
  funcs->glDisable(GL_BLEND);
  funcs->glDisable(GL_DEPTH_TEST);
  funcs->glDepthMask(GL_TRUE);
  funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  scene_fbo_.resolve();

  // SSAO over the resolved depth (Phase D). Runs in the post-chain state set
  // above; binds its own FBOs, so it must precede the bindDefault below. The
  // present degrades to no-AO (u_has_ao=0) when the pass is unavailable.
  if (composite_params_.ssao_enabled) {
    ssao_.initializeGL();
    ssao_.resize(device_width_px, device_height_px);
    ssao_.setDepthTexture(scene_fbo_.resolvedDepthTextureId());
    ssao_.renderAo(view_params);
  }
  if (composite_params_.edl_enabled) {
    edl_.initializeGL();
    edl_.resize(device_width_px, device_height_px);
    edl_.setDepthTexture(scene_fbo_.resolvedDepthTextureId());
    edl_.renderEdl(view_params);
  }

  // Present as a fullscreen passthrough DRAW (not a blit): the backing FBO is
  // itself multisampled, and single->MSAA blits are invalid while MSAA->MSAA
  // blits require identical formats (ours is RGBA16F, Qt's is RGBA8). The
  // shader forces alpha to 1.0, which keeps the Wayland opaque-surface
  // invariant on this path (the role the old geometry-phase glColorMask guard
  // played when geometry still wrote the backing FBO directly).
  gl::Framebuffer::bindDefault(defaultFramebufferObject());
  funcs->glViewport(0, 0, device_width_px, device_height_px);
  // u_scene/u_depth/u_ao/u_edl sampler units are constant and were set once at
  // program build (initializePresentProgram); only the binds and flags vary here.
  present_program_->use();
  funcs->glActiveTexture(GL_TEXTURE0);
  funcs->glBindTexture(GL_TEXTURE_2D, scene_fbo_.resolvedColorTextureId());
  funcs->glActiveTexture(GL_TEXTURE1);
  funcs->glBindTexture(GL_TEXTURE_2D, scene_fbo_.resolvedDepthTextureId());
  const bool ao_active = composite_params_.ssao_enabled && ssao_.ready();
  present_program_->setInt("u_has_ao", ao_active ? 1 : 0);
  if (ao_active) {
    funcs->glActiveTexture(GL_TEXTURE2);
    funcs->glBindTexture(GL_TEXTURE_2D, ssao_.outputTextureId());
  }
  const bool edl_active = composite_params_.edl_enabled && edl_.ready();
  present_program_->setInt("u_has_edl", edl_active ? 1 : 0);
  if (edl_active) {
    funcs->glActiveTexture(GL_TEXTURE3);
    funcs->glBindTexture(GL_TEXTURE_2D, edl_.outputTextureId());
  }
  present_program_->setInt("u_tonemap_mode", composite_params_.tonemap_mode);
  present_program_->setFloat("u_exposure", composite_params_.exposure);
  present_program_->setFloat("u_saturation", composite_params_.saturation);
  present_program_->setFloat("u_ao_strength", composite_params_.ao_strength);
  present_vao_.bind();
  funcs->glDrawArrays(GL_TRIANGLES, 0, 3);
  present_vao_.unbind();
  funcs->glActiveTexture(GL_TEXTURE1);
  funcs->glBindTexture(GL_TEXTURE_2D, 0);
  funcs->glActiveTexture(GL_TEXTURE0);
  funcs->glBindTexture(GL_TEXTURE_2D, 0);

  // Leave depth/blend enabled — the state the legacy path ended each frame with.
  funcs->glEnable(GL_DEPTH_TEST);
  funcs->glEnable(GL_BLEND);
}

void SceneViewWidget::renderScene(
    const ViewParams& view_params, const FrameContext& frame_ctx, bool mask_alpha_writes) {
  auto* ctx = QOpenGLContext::currentContext();
  auto* funcs = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(ctx);
  if (funcs == nullptr) {
    return;
  }

  // Theme-aware background + grid — see Phase 1 commit (theme-aware
  // background + high-contrast grid color) for the full rationale.
  // Read the APPLICATION palette, not this widget's: QStyleSheetStyle's polish
  // rewrites widget palettes from QSS rules (the app stylesheet's transparent
  // QWidget background lands as #000000 in QPalette::Window), while the
  // application palette is kept in lockstep with the theme by pj_app's Theme.
  const QPalette pal = QGuiApplication::palette();
  const QColor window_bg = pal.color(QPalette::Window);
  const bool dark_theme = window_bg.valueF() < 0.5F;
  const QColor bg = dark_theme ? QColor(45, 48, 56) : QColor(255, 255, 255);
  const QColor fg = dark_theme ? QColor(220, 220, 220) : QColor(40, 40, 40);
  // Off-screen path: the scene FBO is linear-light (the composite present
  // re-encodes to sRGB), so display-referred theme colors must be linearized on
  // write. The direct-to-backing fallback has no encode — leave them as-is there.
  const bool linear_target = !mask_alpha_writes;
  const auto lin = [linear_target](qreal c) {
    return linear_target ? static_cast<float>(std::pow(c, 2.2)) : static_cast<float>(c);
  };
  funcs->glClearColor(lin(bg.redF()), lin(bg.greenF()), lin(bg.blueF()), 1.0f);
  constexpr float kGridBlend = 0.35f;
  grid_.setColor(
      glm::vec3{
          lin(bg.redF() * (1.0 - kGridBlend) + fg.redF() * kGridBlend),
          lin(bg.greenF() * (1.0 - kGridBlend) + fg.greenF() * kGridBlend),
          lin(bg.blueF() * (1.0 - kGridBlend) + fg.blueF() * kGridBlend),
      });
  funcs->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  funcs->glEnable(GL_DEPTH_TEST);
  funcs->glEnable(GL_BLEND);
  // The scene FBO's alpha is the per-pixel tonemap marker (1 = data, graded;
  // 0 = annotation, raw). Data draws must RESTORE it — coverage-union alpha
  // (ONE, ONE_MINUS_SRC_ALPHA): an opaque draw stamps 1, a translucent draw
  // raises it proportionally, untouched annotation pixels keep 0. Preserving
  // destination alpha instead (ZERO, ONE) left stale alpha-0 from occluded TF
  // arrows under the robot body, ghosting raw arrow shapes through the mesh.
  funcs->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  if (mask_alpha_writes) {
    // Direct-to-backing path only. The 3D viewport is opaque: the clear above
    // set the framebuffer alpha to 1.0, and masking alpha writes keeps it there
    // through every pass. RGB still blends normally (src.a is the blend
    // *factor*, not an alpha write), so transparent content like the occupancy
    // grid (opacity < 1) looks correct — but no pass can lower the framebuffer's
    // alpha. Without this, a sub-1.0 alpha left in the QOpenGLWidget's FBO makes
    // the Wayland compositor treat those regions as translucent and bleed the
    // previous frame through them. The caller restores the mask after rendering
    // so the next frame's glClear can repaint alpha (glClear honours the mask).
    funcs->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  }

  // Annotation blend mode (axes + HUD): frag alpha is the gizmo opacity; RGB
  // blends normally while the alpha factors (ZERO, 1-SRC_ALPHA) DECREASE the
  // tonemap-bypass marker by the annotation's coverage — at opacity 1 this is
  // identical to the old blend-off + alpha-0 write (RGB=src, marker→0).
  const auto annotation_blend = [funcs] {
    funcs->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
  };
  const auto data_blend = [funcs] {
    funcs->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  };

  if (grid_visible_) {
    grid_.render(view_params, frame_ctx);
  }
  if (tf_ && axes_visible_) {
    annotation_blend();
    axes_.render(view_params, frame_ctx);
    data_blend();
  }
  // Iterate layers in the order supplied by SceneDockWidget. Each layer is responsible
  // for its own GL state — initializeGL() is intentionally called per
  // frame, and layers (like the render passes they own) guard
  // against double-init via an internal `initialized_` flag. The
  // per-frame call lets layers added *after* the widget realises
  // initialise on their first paint without needing a current GL
  // context at attach time. See Scene3DLayer::initializeGL for the
  // contract.
  if (tf_) {
    for (Scene3DLayer* layer : layers_) {
      if (layer == nullptr) {
        continue;
      }
      // Re-assert the ambient blend state before each layer (defense in depth):
      // a pass that leaks a different blend func/enable (H.10/M.31) can't then
      // poison the rest of the frame, so each layer starts from the data
      // contract regardless of what the previous one left behind.
      funcs->glEnable(GL_BLEND);
      data_blend();
      layer->initializeGL();
      layer->render(view_params, frame_ctx);
    }
  }

  // Camera-orientation HUD (top-right by default). Drawn last so the solid
  // arrows sit on top of every scene-space pass. Annotation, opacity 1.
  annotation_blend();
  overlay_.render(view_params, frame_ctx);
  data_blend();
}

void SceneViewWidget::setCameraModel(CameraModel model) {
  const CameraState carried = camera_->state();
  std::unique_ptr<ICamera> next;
  switch (model) {
    case CameraModel::Orbit:
      next = std::make_unique<OrbitCamera>();
      break;
    case CameraModel::TopDownOrtho:
      next = std::make_unique<TopDownOrthoCamera>();
      break;
    case CameraModel::Fly:
      next = std::make_unique<FlyCamera>();
      break;
    case CameraModel::XYOrbit:
      next = std::make_unique<XYOrbitCamera>();
      break;
  }
  next->adoptState(carried);            // carry the pose across so the view doesn't jump
  next->setSceneBounds(scene_bounds_);  // bounds aren't part of CameraState
  camera_ = std::move(next);
  update();
}

void SceneViewWidget::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
  camera_->setSceneBounds(bounds);
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_pos_ = event->position().toPoint();
  active_button_ = event->button();
}

void SceneViewWidget::mouseReleaseEvent(QMouseEvent* event) {
  // Clear the active-gesture latch when its button is released so a chorded
  // drag (e.g. press LMB then MMB, release MMB, keep dragging LMB) doesn't keep
  // applying the released button's gesture.
  if (event->button() == active_button_) {
    active_button_ = Qt::NoButton;
  }
}

void SceneViewWidget::mouseMoveEvent(QMouseEvent* event) {
  if (active_button_ == Qt::NoButton) {
    return;
  }
  const QPoint current = event->position().toPoint();
  const QPoint delta = current - last_mouse_pos_;
  last_mouse_pos_ = current;

  const float dx = static_cast<float>(delta.x());
  const float dy = static_cast<float>(delta.y());
  const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;

  if (active_button_ == Qt::LeftButton && !shift) {
    camera_->rotate(dx, dy);
  } else if (active_button_ == Qt::MiddleButton || (active_button_ == Qt::LeftButton && shift)) {
    camera_->pan(dx, dy);
  } else if (active_button_ == Qt::RightButton) {
    // Right-drag stays center-of-view zoom — cursor-anchoring per drag delta
    // walks the focal (focal creep); only the wheel is cursor-anchored.
    camera_->zoom(dy * 0.01f);
  }

  update();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0f;
  const QPointF pos = event->position();
  camera_->zoomToCursor(ticks, glm::vec2{static_cast<float>(pos.x()), static_cast<float>(pos.y())}, width(), height());
  update();
}

void SceneViewWidget::changeEvent(QEvent* event) {
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
    update();
  }
  QOpenGLWidget::changeEvent(event);
}

}  // namespace pj::scene3d
