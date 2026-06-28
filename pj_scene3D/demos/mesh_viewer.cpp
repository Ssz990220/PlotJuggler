// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Standalone URDF mesh viewer — a fast look-development loop for the 3D mesh
// pipeline (textures, tonemap, SSAO, lighting) without loading an MCAP.
//
//   scene3d_mesh_viewer <robot.urdf> [mesh-search-root]
//
// Renders through the REAL production path: RobotModelLayer (File source) into
// SceneViewWidget (HDR chain -> SSAO -> composite). The only fakery is the TF
// tree: the app parses the URDF's <joint> origins into a static zero-pose
// hierarchy (every joint at its neutral origin) — the product never does this
// (TF from data is truth there), but a bag-free viewer must pose links somehow.
// Mesh refs resolve exactly as in the app (ancestor heuristic from the URDF's
// directory; pass a search root for detached layouts).

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDomDocument>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QSettings>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <memory>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "urdf_package_resolver.h"  // private widgets/src header, like the tests

namespace {

namespace look = pj::scene3d::look;  // canonical look defaults (scene_look_defaults.h)

glm::dvec3 parseTriple(const QString& text) {
  const QStringList parts = text.split(' ', Qt::SkipEmptyParts);
  glm::dvec3 v{0.0, 0.0, 0.0};
  for (int i = 0; i < parts.size() && i < 3; ++i) {
    v[i] = parts[i].toDouble();
  }
  return v;
}

// URDF rpy convention: R = Rz(yaw) * Ry(pitch) * Rx(roll).
glm::dquat quatFromRpy(const glm::dvec3& rpy) {
  const glm::dquat qx = glm::angleAxis(rpy.x, glm::dvec3{1.0, 0.0, 0.0});
  const glm::dquat qy = glm::angleAxis(rpy.y, glm::dvec3{0.0, 1.0, 0.0});
  const glm::dquat qz = glm::angleAxis(rpy.z, glm::dvec3{0.0, 0.0, 1.0});
  return qz * qy * qx;
}

// Seed the buffer with the URDF's zero-configuration pose: one static
// parent->child transform per <joint>, taken from its <origin>. All joint
// types are treated as fixed at their neutral position.
int seedZeroPoseTf(const QString& urdf_path, pj::scene3d::TransformBuffer& tf) {
  QFile file(urdf_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    std::fprintf(stderr, "cannot read %s\n", qPrintable(urdf_path));
    return 0;
  }
  QDomDocument doc;
  if (!doc.setContent(&file)) {
    std::fprintf(stderr, "not parseable XML: %s\n", qPrintable(urdf_path));
    return 0;
  }
  int joints = 0;
  for (QDomElement joint = doc.documentElement().firstChildElement(QStringLiteral("joint")); !joint.isNull();
       joint = joint.nextSiblingElement(QStringLiteral("joint"))) {
    const QString parent = joint.firstChildElement(QStringLiteral("parent")).attribute(QStringLiteral("link"));
    const QString child = joint.firstChildElement(QStringLiteral("child")).attribute(QStringLiteral("link"));
    if (parent.isEmpty() || child.isEmpty()) {
      continue;
    }
    const QDomElement origin = joint.firstChildElement(QStringLiteral("origin"));
    const glm::dvec3 xyz = parseTriple(origin.attribute(QStringLiteral("xyz")));
    const glm::dvec3 rpy = parseTriple(origin.attribute(QStringLiteral("rpy")));
    const auto result = tf.setTransform(
        pj::scene3d::StampedTransform{
            .stamp = PJ::fromRaw(0),
            .parent_frame = parent.toStdString(),
            .child_frame = child.toStdString(),
            .transform = pj::scene3d::Transform(xyz, quatFromRpy(rpy)),
        });
    if (result.has_value()) {
      ++joints;
    }
  }
  return joints;
}

QString rootLinkName(const QString& urdf_path) {
  // First <link> declared == the parser's root inference; good enough here.
  QFile file(urdf_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  QDomDocument doc;
  if (!doc.setContent(&file)) {
    return {};
  }
  return doc.documentElement().firstChildElement(QStringLiteral("link")).attribute(QStringLiteral("name"));
}

// Slider mapping value/100 -> float, with a live value label.
QSlider* addSlider(
    QFormLayout* form, const QString& label, int min, int max, int value, const std::function<void(float)>& apply) {
  auto* slider = new QSlider(Qt::Horizontal);
  slider->setRange(min, max);
  slider->setValue(value);
  auto* name = new QLabel(QStringLiteral("%1 (%2)").arg(label).arg(value / 100.0));
  QObject::connect(slider, &QSlider::valueChanged, name, [name, label, apply](int v) {
    name->setText(QStringLiteral("%1 (%2)").arg(label).arg(v / 100.0));
    apply(static_cast<float>(v) / 100.0f);
  });
  form->addRow(name, slider);
  return slider;
}

// Look-development control panel: the knobs most relevant to dialing in the
// scene appearance (composite, SSAO, mesh shading).
QWidget* makeControls(pj::scene3d::SceneViewWidget& view) {
  auto* panel = new QWidget;
  auto* form = new QFormLayout(panel);
  form->setContentsMargins(8, 8, 8, 8);
  const auto repaint = [&view] { view.update(); };

  // ---- Anti-aliasing + live perf HUD (what this view exists to explore) ------
  // A repaint timer keeps the scene re-rendering while the HUD is on so the
  // GPU/CPU numbers tick live and settle (a single frame's GPU time is noisy).
  auto* live_timer = new QTimer(&view);
  live_timer->setInterval(16);  // ~60 Hz request; vsync caps the real rate
  QObject::connect(live_timer, &QTimer::timeout, &view, qOverload<>(&QWidget::update));

  auto* hud_box = new QCheckBox(QStringLiteral("Perf HUD — live GPU/CPU ms (key: P)"));
  hud_box->setChecked(true);
  view.setShowPerfHud(true);
  live_timer->start();
  QObject::connect(hud_box, &QCheckBox::toggled, &view, [&view, live_timer](bool on) {
    view.setShowPerfHud(on);
    if (on) {
      live_timer->start();
    } else {
      live_timer->stop();
      view.update();  // one repaint to clear the overlay
    }
  });
  form->addRow(hud_box);

  // MSAA: anti-aliases geometry silhouettes only (4 / 8 coverage steps). Cheap;
  // does nothing for in-triangle specular shimmer. Index i -> 2^i samples.
  auto* msaa = new QComboBox;
  msaa->addItems({QStringLiteral("Off (1x)"), QStringLiteral("2x"), QStringLiteral("4x"), QStringLiteral("8x")});
  msaa->setCurrentIndex(2);  // 4x — matches the app default
  view.setSceneSamples(4);
  QObject::connect(msaa, &QComboBox::currentIndexChanged, &view, [&view](int idx) { view.setSceneSamples(1 << idx); });
  form->addRow(QStringLiteral("MSAA"), msaa);

  // Supersample (SSAA): render the scene at scale x device px and downsample.
  // Anti-aliases BOTH silhouettes and shading; cost grows ~scale^2. 1.0 = off.
  // Tip: with SSAA > 1, drop MSAA to Off — SSAA already covers edges and the
  // MSAA resolve at supersampled resolution is pure waste (watch the HUD).
  addSlider(form, QStringLiteral("Supersample"), 100, 200, 100, [&view](float v) { view.setRenderScale(v); });

  auto* tonemap = new QComboBox;
  tonemap->addItems({QStringLiteral("None"), QStringLiteral("ACES"), QStringLiteral("AgX"), QStringLiteral("Neutral")});
  tonemap->setCurrentIndex(view.compositeParams().tonemap_mode);
  QObject::connect(tonemap, &QComboBox::currentIndexChanged, &view, [&view, repaint](int idx) {
    view.compositeParams().tonemap_mode = idx;
    repaint();
  });
  form->addRow(QStringLiteral("Tonemap"), tonemap);

  addSlider(
      form, QStringLiteral("Exposure"), 25, 400, static_cast<int>(view.compositeParams().exposure * 100),
      [&view, repaint](float v) {
        view.compositeParams().exposure = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Saturation"), 0, 250, static_cast<int>(view.compositeParams().saturation * 100),
      [&view, repaint](float v) {
        view.compositeParams().saturation = v;
        repaint();
      });

  auto* ssao_box = new QCheckBox(QStringLiteral("SSAO"));
  ssao_box->setChecked(view.compositeParams().ssao_enabled);
  QObject::connect(ssao_box, &QCheckBox::toggled, &view, [&view, repaint](bool on) {
    view.compositeParams().ssao_enabled = on;
    repaint();
  });
  form->addRow(ssao_box);
  addSlider(
      form, QStringLiteral("AO strength"), 0, 100, static_cast<int>(view.compositeParams().ao_strength * 100),
      [&view, repaint](float v) {
        view.compositeParams().ao_strength = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("AO radius m"), 5, 200, static_cast<int>(look::kSsaoRadiusM * 100),
      [&view, repaint](float v) {
        view.ssaoPass().setRadius(v);
        repaint();
      });
  addSlider(
      form, QStringLiteral("AO power"), 50, 500, static_cast<int>(look::kSsaoPower * 100), [&view, repaint](float v) {
        view.ssaoPass().setPower(v);
        repaint();
      });

  auto* edl_box = new QCheckBox(QStringLiteral("EDL"));
  edl_box->setChecked(view.compositeParams().edl_enabled);
  QObject::connect(edl_box, &QCheckBox::toggled, &view, [&view, repaint](bool on) {
    view.compositeParams().edl_enabled = on;
    repaint();
  });
  form->addRow(edl_box);
  addSlider(
      form, QStringLiteral("EDL strength"), 0, 400, static_cast<int>(look::kEdlStrength * 100),
      [&view, repaint](float v) {
        view.edlPass().setStrength(v);
        repaint();
      });
  addSlider(
      form, QStringLiteral("EDL radius px"), 10, 500, static_cast<int>(look::kEdlRadiusPx * 100),
      [&view, repaint](float v) {
        view.edlPass().setRadiusPx(v);
        repaint();
      });
  addSlider(
      form, QStringLiteral("EDL max gap"), 0, 20, static_cast<int>(look::kEdlMaxGap * 100), [&view, repaint](float v) {
        view.edlPass().setMaxGap(v);
        repaint();
      });
  addSlider(
      form, QStringLiteral("EDL floor"), 0, 100, static_cast<int>(view.compositeParams().edl_floor * 100),
      [&view, repaint](float v) {
        view.compositeParams().edl_floor = v;
        repaint();
      });

  // Per-view shading knobs: `view` outlives this panel (both owned by `window`),
  // so the lambdas can capture &shading safely, same as the compositeParams() rows.
  auto& shading = view.meshShadingParams();
  addSlider(
      form, QStringLiteral("Roughness"), 5, 100, static_cast<int>(shading.roughness * 100),
      [&shading, repaint](float v) {
        shading.roughness = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Reflectivity"), 0, 25, static_cast<int>(shading.reflectivity * 100),
      [&shading, repaint](float v) {
        shading.reflectivity = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Ambient"), 0, 250, static_cast<int>(shading.ambient_scale * 100),
      [&shading, repaint](float v) {
        shading.ambient_scale = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Key light"), 0, 250, static_cast<int>(shading.direct_scale * 100),
      [&shading, repaint](float v) {
        shading.direct_scale = v;
        repaint();
      });
  addSlider(
      form, QStringLiteral("Fill light"), 0, 150, static_cast<int>(shading.fill_light_scale * 100),
      [&shading, repaint](float v) {
        shading.fill_light_scale = v;
        repaint();
      });

  // Key-light direction as azimuth/elevation (degrees), shared by both sliders.
  // Seeded from the canonical defaults so the sliders start matched to the struct.
  auto key_angles = std::make_shared<glm::vec2>(look::kKeyLightAzimuthDeg, look::kKeyLightElevationDeg);
  const auto apply_key_dir = [&shading, key_angles, repaint] {
    shading.key_light_dir = look::keyDirFromAzEl(key_angles->x, key_angles->y);
    repaint();
  };
  const auto add_deg_slider =
      [form](const QString& label, int min, int max, int value, const std::function<void(float)>& apply) {
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(min, max);
        slider->setValue(value);
        auto* name = new QLabel(QStringLiteral("%1 (%2°)").arg(label).arg(value));
        QObject::connect(slider, &QSlider::valueChanged, name, [name, label, apply](int v) {
          name->setText(QStringLiteral("%1 (%2°)").arg(label).arg(v));
          apply(static_cast<float>(v));
        });
        form->addRow(name, slider);
      };
  add_deg_slider(
      QStringLiteral("Key azimuth"), -180, 180, static_cast<int>(look::kKeyLightAzimuthDeg),
      [key_angles, apply_key_dir](float v) {
        key_angles->x = v;
        apply_key_dir();
      });
  add_deg_slider(
      QStringLiteral("Key elevation"), 0, 90, static_cast<int>(look::kKeyLightElevationDeg),
      [key_angles, apply_key_dir](float v) {
        key_angles->y = v;
        apply_key_dir();
      });

  addSlider(
      form, QStringLiteral("Env reflection"), 0, 200, static_cast<int>(shading.env_intensity * 100),
      [&shading, repaint](float v) {
        shading.env_intensity = v;
        repaint();
      });

  auto* axes_box = new QCheckBox(QStringLiteral("TF axes"));
  axes_box->setChecked(false);
  QObject::connect(axes_box, &QCheckBox::toggled, &view, [&view](bool on) { view.setAxesVisible(on); });
  form->addRow(axes_box);

  panel->setFixedWidth(300);
  return panel;
}

// Parsed command line. Positionals: <robot.urdf> [mesh-search-root]. Flags let a
// script capture each look variant headlessly without driving the panel.
struct CliOptions {
  QString urdf;
  QString search_root;
  QString screenshot_path;                                 // empty => interactive (no auto-capture)
  int delay_ms = 3000;                                     // wait for async mesh load before grabbing
  int tonemap = -1;                                        // <0: keep default (0 None,1 ACES,2 AgX,3 Neutral)
  float env = -1.0f;                                       // <0: keep default env-reflection intensity
  float key_az = std::numeric_limits<float>::quiet_NaN();  // key-light azimuth deg
  float key_el = std::numeric_limits<float>::quiet_NaN();  // key-light elevation deg
  bool benchmark = false;                                  // run the anti-aliasing GPU-cost sweep, print a table, exit
  int bench_frames = 150;                                  // timed frames collected per config
  QString bench_csv;                                       // optional CSV dump of the per-config medians
  int msaa = -1;                                           // <0: keep default; else off-screen MSAA samples (1/2/4/8)
  float ssaa = -1.0f;      // <0: keep default; else supersampling render scale (e.g. 1.5)
  bool shadows = false;    // mesh-only directional shadows (off by default, opt-in)
  bool collisions = true;  // show collision hulls (set off to inspect visual meshes alone)
  float cam_radius = std::numeric_limits<float>::quiet_NaN();  // orbit distance override (m)
  float cam_az = std::numeric_limits<float>::quiet_NaN();      // orbit azimuth override (deg)
  float cam_el = std::numeric_limits<float>::quiet_NaN();      // orbit elevation override (deg)
  float cam_focal_z = 0.4f;                                    // look-at height (m)
  int win_w = -1;                                              // <0: keep default 1500x840 window
  int win_h = -1;  // else force the main window size (view ~= this minus panel)
};

CliOptions parseCli(const QStringList& args) {
  CliOptions opts;
  // args[0] is the program name; flags consume the following token as their value.
  for (int i = 1; i < args.size(); ++i) {
    const QString& arg = args[i];
    const auto next = [&args, &i]() -> QString { return ++i < args.size() ? args[i] : QString(); };
    if (arg == QStringLiteral("--screenshot")) {
      opts.screenshot_path = next();
    } else if (arg == QStringLiteral("--benchmark")) {
      opts.benchmark = true;
    } else if (arg == QStringLiteral("--bench-frames")) {
      opts.bench_frames = next().toInt();
    } else if (arg == QStringLiteral("--bench-csv")) {
      opts.bench_csv = next();
    } else if (arg == QStringLiteral("--win-size")) {
      const QStringList wh = next().split('x', Qt::SkipEmptyParts);
      if (wh.size() == 2) {
        opts.win_w = wh[0].toInt();
        opts.win_h = wh[1].toInt();
      }
    } else if (arg == QStringLiteral("--msaa")) {
      opts.msaa = next().toInt();
    } else if (arg == QStringLiteral("--ssaa")) {
      opts.ssaa = next().toFloat();
    } else if (arg == QStringLiteral("--delay-ms")) {
      opts.delay_ms = next().toInt();
    } else if (arg == QStringLiteral("--tonemap")) {
      opts.tonemap = next().toInt();
    } else if (arg == QStringLiteral("--env")) {
      opts.env = next().toFloat();
    } else if (arg == QStringLiteral("--key-az")) {
      opts.key_az = next().toFloat();
    } else if (arg == QStringLiteral("--key-el")) {
      opts.key_el = next().toFloat();
    } else if (arg == QStringLiteral("--shadows")) {
      opts.shadows = next() != QStringLiteral("off");  // "--shadows on" / "--shadows off"
    } else if (arg == QStringLiteral("--collisions")) {
      opts.collisions = next() != QStringLiteral("off");
    } else if (arg == QStringLiteral("--cam-radius")) {
      opts.cam_radius = next().toFloat();
    } else if (arg == QStringLiteral("--cam-az")) {
      opts.cam_az = next().toFloat();
    } else if (arg == QStringLiteral("--cam-el")) {
      opts.cam_el = next().toFloat();
    } else if (arg == QStringLiteral("--cam-focal-z")) {
      opts.cam_focal_z = next().toFloat();
    } else if (opts.urdf.isEmpty()) {
      opts.urdf = arg;
    } else if (opts.search_root.isEmpty()) {
      opts.search_root = arg;
    }
  }
  return opts;
}

// ---- Anti-aliasing GPU-cost benchmark --------------------------------------

// Linear-interpolated percentile of a sample set (pct in [0,1]). Sorts a copy.
double percentile(std::vector<double> samples, double pct) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double pos = pct * static_cast<double>(samples.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(pos));
  const auto hi = static_cast<std::size_t>(std::ceil(pos));
  if (lo == hi) {
    return samples[lo];
  }
  return samples[lo] + (samples[hi] - samples[lo]) * (pos - static_cast<double>(lo));
}

// Approximate VRAM of the off-screen scene chain at (w x h) render pixels:
// resolved RGBA16F color (8B/px) + D32F depth (4B/px), plus the multisample
// color+depth attachments (samples x each) when MSAA is active. This is the
// dominant, SSAA-scaling term; SSAO/EDL add a little more on top.
double sceneFboMegabytes(int width_px, int height_px, int samples) {
  const double px = static_cast<double>(width_px) * static_cast<double>(height_px);
  double bytes = px * 8.0 + px * 4.0;
  if (samples > 1) {
    bytes += (px * 8.0 + px * 4.0) * static_cast<double>(samples);
  }
  return bytes / (1024.0 * 1024.0);
}

struct BenchConfig {
  int samples;  // requested off-screen MSAA (1 disables MSAA)
  float scale;  // supersampling factor (1.0 = native)
};

// Sweep {MSAA 1,2,4,8} x {scale 1.0,1.5,2.0}, driven off the view's
// frameSwapped signal. Per config: discard `warmup` frames (GL_TIME_ELAPSED
// results read back a few frames late), collect `frames` GPU/CPU samples, print
// the row, advance. Quits when the sweep is exhausted. State lives on the heap,
// kept alive by the lambda captured into the connection.
void runBenchmark(pj::scene3d::SceneViewWidget* view, const CliOptions& opts) {
  struct State {
    std::vector<BenchConfig> configs;
    std::size_t idx = 0;
    int warmup = 24;
    int frames = 150;
    int warmup_left = 0;
    int measure_left = 0;
    std::vector<double> gpu;
    std::vector<double> cpu;
    int dev_w = 0;
    int dev_h = 0;
    QString csv;
    std::vector<QString> csv_rows;
  };
  auto state = std::make_shared<State>();
  state->warmup = 24;  // discarded per config; covers the GPU readback latency
  state->frames = std::max(8, opts.bench_frames);
  state->csv = opts.bench_csv;
  for (int samples : {1, 2, 4, 8}) {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
      state->configs.push_back(BenchConfig{samples, scale});
    }
  }

  const qreal dpr = view->devicePixelRatioF();
  state->dev_w = static_cast<int>(std::lround(view->width() * dpr));
  state->dev_h = static_cast<int>(std::lround(view->height() * dpr));

  std::printf("\n[mesh_viewer] anti-aliasing benchmark — %s\n", qPrintable(QFileInfo(opts.urdf).fileName()));
  std::printf(
      "  device %dx%d (DPR %.2f)   vsync OFF   frames/config %d (warmup %d)\n", state->dev_w, state->dev_h,
      static_cast<double>(dpr), state->frames, state->warmup);
  std::printf("  GPU ms = GL_TIME_ELAPSED (moving avg); CPU ms = paintGL submission (should stay flat)\n\n");
  std::printf("  MSAA  SSAA   GPU med   GPU p95   CPU med   sceneFBO\n");
  std::printf("  ----  -----  --------  --------  --------  --------\n");
  std::fflush(stdout);
  if (!state->csv.isEmpty()) {
    state->csv_rows.push_back(QStringLiteral("msaa,ssaa,gpu_med_ms,gpu_p95_ms,cpu_med_ms,scenefbo_mb"));
  }

  const auto apply = [view](const BenchConfig& cfg) {
    view->setSceneSamples(cfg.samples);
    view->setRenderScale(cfg.scale);
  };

  // The GPU/CPU profiler is gated on the perf HUD; enable it so the sweep
  // collects timings (the overlay also draws on the benchmark frames — harmless).
  view->setShowPerfHud(true);
  apply(state->configs[0]);
  state->warmup_left = state->warmup;
  state->measure_left = state->frames;

  QObject::connect(view, &QOpenGLWidget::frameSwapped, view, [view, state, apply]() {
    if (state->idx >= state->configs.size()) {
      return;
    }
    if (state->warmup_left > 0) {
      --state->warmup_left;
    } else if (state->measure_left > 0) {
      // Only count a frame once a fresh GPU result is available (post-warmup it
      // always is); otherwise keep spinning without consuming the budget.
      if (view->hasGpuResult()) {
        state->gpu.push_back(view->gpuFrameMillis());
        state->cpu.push_back(view->cpuFrameMillis());
        --state->measure_left;
      }
    } else {
      const BenchConfig& cfg = state->configs[state->idx];
      const int got_samples = view->achievedSceneSamples();
      const double gpu_med = percentile(state->gpu, 0.5);
      const double gpu_p95 = percentile(state->gpu, 0.95);
      const double cpu_med = percentile(state->cpu, 0.5);
      const int eff_w = static_cast<int>(std::lround(static_cast<float>(state->dev_w) * cfg.scale));
      const int eff_h = static_cast<int>(std::lround(static_cast<float>(state->dev_h) * cfg.scale));
      const double mb = sceneFboMegabytes(eff_w, eff_h, got_samples);
      std::printf(
          "  %3dx  %4.1fx  %7.2f   %7.2f   %7.2f   %5.0f MB\n", got_samples, static_cast<double>(cfg.scale), gpu_med,
          gpu_p95, cpu_med, mb);
      std::fflush(stdout);
      if (!state->csv.isEmpty()) {
        state->csv_rows.push_back(QStringLiteral("%1,%2,%3,%4,%5,%6")
                                      .arg(got_samples)
                                      .arg(static_cast<double>(cfg.scale))
                                      .arg(gpu_med, 0, 'f', 3)
                                      .arg(gpu_p95, 0, 'f', 3)
                                      .arg(cpu_med, 0, 'f', 3)
                                      .arg(mb, 0, 'f', 1));
      }

      ++state->idx;
      if (state->idx >= state->configs.size()) {
        std::printf("\n[mesh_viewer] benchmark done.\n");
        if (!state->csv.isEmpty()) {
          QFile csv_file(state->csv);
          if (csv_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            for (const QString& row : state->csv_rows) {
              csv_file.write(row.toUtf8());
              csv_file.write("\n");
            }
            std::printf("[mesh_viewer] csv: %s\n", qPrintable(state->csv));
          }
        }
        std::fflush(stdout);
        QApplication::quit();
        return;
      }
      apply(state->configs[state->idx]);
      state->gpu.clear();
      state->cpu.clear();
      state->warmup_left = state->warmup;
      state->measure_left = state->frames;
    }
    view->update();  // free-run the next frame (vsync off)
  });

  view->update();  // start the loop
}

}  // namespace

constexpr const char* kUsage =
    "usage: scene3d_mesh_viewer <robot.urdf> [mesh-search-root]\n"
    "  --screenshot <path>   render, save a PNG after --delay-ms, then exit\n"
    "  --delay-ms <n>        wait before the screenshot grab / benchmark start (default 3000)\n"
    "  --benchmark           sweep MSAA x supersampling, print a GPU/CPU cost table, exit\n"
    "  --bench-frames <n>    timed frames per config (default 150)\n"
    "  --bench-csv <path>    also write the per-config medians as CSV\n"
    "  --msaa <1|2|4|8>      preset off-screen MSAA samples\n"
    "  --ssaa <scale>        preset supersampling render scale (e.g. 1.5, 2.0)\n"
    "  --tonemap <0..3>      0 None, 1 ACES, 2 AgX, 3 Neutral\n"
    "  --env <f>             env-reflection (analytic IBL) intensity\n"
    "  --key-az <deg>        key-light azimuth      --key-el <deg> elevation\n";

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  // Own QSettings scope (separate ini from the real app) for the persisted camera.
  QApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QApplication::setApplicationName(QStringLiteral("scene3d_mesh_viewer"));
  const CliOptions opts = parseCli(app.arguments());

  QString urdf_arg = opts.urdf;
  if (urdf_arg.isEmpty()) {
    if (!opts.screenshot_path.isEmpty()) {
      std::fprintf(stderr, "screenshot mode requires a URDF path\n%s", kUsage);
      return 1;
    }
    urdf_arg = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("Open URDF"), QString(), QStringLiteral("URDF files (*.urdf *.xml);;All files (*)"));
    if (urdf_arg.isEmpty()) {
      std::fprintf(stderr, "%s", kUsage);
      return 1;
    }
  }
  const QString urdf_path = QFileInfo(urdf_arg).absoluteFilePath();

  PJ::SessionManager session;
  auto tf = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
  const int joints = seedZeroPoseTf(urdf_path, *tf);
  const QString root = rootLinkName(urdf_path);
  std::printf(
      "[mesh_viewer] %s: %d joints seeded at zero pose, root link '%s'\n", qPrintable(urdf_path), joints,
      qPrintable(root));

  pj::scene3d::UrdfPackageResolver resolver;
  if (!opts.search_root.isEmpty()) {
    resolver.addSearchRoot(opts.search_root);
  }

  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{.id = 1}, QStringLiteral("mesh_viewer"));
  layer.setPackageResolver(&resolver);
  // Force the File source BEFORE attach so the synthetic topic id never
  // reaches the ObjectStore (the dock's local-layer rule, mirrored here).
  layer.setSourceFile(QString());
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = tf;
  layer.attach(ctx);
  layer.setSourceFile(urdf_path);

  QWidget window;
  window.setWindowTitle(QStringLiteral("scene3d_mesh_viewer — %1").arg(QFileInfo(urdf_path).fileName()));
  auto* row = new QHBoxLayout(&window);
  row->setContentsMargins(0, 0, 0, 0);
  auto* view = new pj::scene3d::SceneViewWidget(&window);
  view->setTransformBuffer(tf);
  view->setAxesVisible(false);  // look-dev default: meshes only (panel toggle)
  view->setFixedFrame(root.toStdString());
  view->setLayers({&layer});
  view->setTrackerTime(PJ::fromRaw(0));
  QObject::connect(&layer, &pj::scene3d::Scene3DLayer::repaintRequested, view, qOverload<>(&QWidget::update));

  // Camera persistence: restore the saved pose so framing is stable across runs
  // (this also keeps --screenshot captures pixel-aligned for A/B comparisons).
  // Save only on a clean interactive quit — a --screenshot run must NOT overwrite
  // the saved framing with its transient default.
  QSettings settings;
  const QString saved_camera = settings.value(QStringLiteral("camera_state")).toString();
  if (!saved_camera.isEmpty()) {
    view->camera().adoptState(pj::scene3d::cameraStateFromJson(saved_camera.toStdString(), view->camera().state()));
  }
  if (!std::isnan(opts.cam_radius) || !std::isnan(opts.cam_az) || !std::isnan(opts.cam_el)) {
    pj::scene3d::CameraState cs = view->camera().state();
    cs.perspective = true;
    cs.focal = glm::vec3(0.0f, 0.0f, opts.cam_focal_z);
    if (!std::isnan(opts.cam_radius)) {
      cs.radius = opts.cam_radius;
    }
    if (!std::isnan(opts.cam_az)) {
      cs.azimuth = glm::radians(opts.cam_az);
    }
    if (!std::isnan(opts.cam_el)) {
      cs.elevation = glm::radians(opts.cam_el);
    }
    view->camera().adoptState(cs);
  }
  if (opts.screenshot_path.isEmpty() && !opts.benchmark) {
    QObject::connect(&app, &QApplication::aboutToQuit, view, [view] {
      QSettings save;
      save.setValue(
          QStringLiteral("camera_state"),
          QString::fromStdString(pj::scene3d::cameraStateToJson(view->camera().state())));
    });
  }

  // CLI look overrides, applied before makeControls (so the panel reflects them)
  // and before the first paint / screenshot grab.
  if (opts.tonemap >= 0) {
    view->compositeParams().tonemap_mode = opts.tonemap;
  }
  auto& shading = view->meshShadingParams();
  shading.shadows_enabled = opts.shadows;
  shading.collisions_visible = opts.collisions;
  if (opts.shadows) {
    // Mesh shadows only land legibly on a solid surface — switch the floor to the
    // filled-cell (checkerboard) grid, which is a shadow receiver, so the demo shows
    // the cast shadow on the ground.
    view->setGridStyle(pj::scene3d::GridRenderPass::Style::kFilledCells);
  }
  if (opts.env >= 0.0f) {
    shading.env_intensity = opts.env;
  }
  if (!std::isnan(opts.key_az) || !std::isnan(opts.key_el)) {
    const float az = std::isnan(opts.key_az) ? look::kKeyLightAzimuthDeg : opts.key_az;
    const float el = std::isnan(opts.key_el) ? look::kKeyLightElevationDeg : opts.key_el;
    shading.key_light_dir = look::keyDirFromAzEl(az, el);
  }

  row->addWidget(view, /*stretch=*/1);
  if (!opts.benchmark) {
    // The benchmark needs a stable, full-window view size for reproducible
    // device dims; the look-dev panel only gets in the way there.
    row->addWidget(makeControls(*view));
  }
  // CLI AA presets win over the panel defaults (makeControls seeds MSAA 4x /
  // scale 1.0); applied after it so --msaa/--ssaa drive A/B screenshots.
  if (opts.msaa > 0) {
    view->setSceneSamples(opts.msaa);
  }
  if (opts.ssaa > 0.0f) {
    view->setRenderScale(opts.ssaa);
  }
  window.resize(opts.win_w > 0 ? opts.win_w : 1500, opts.win_h > 0 ? opts.win_h : 840);

  if (opts.benchmark) {
    // Free-run (no vsync) so the sweep measures true GPU cost rather than the
    // 60 Hz cap and finishes in seconds. Must be set before the widget is shown.
    QSurfaceFormat fmt = view->format();
    fmt.setSwapInterval(0);
    view->setFormat(fmt);
  }
  window.show();

  // Headless one-shot capture: render past the async mesh load, save a PNG via
  // QOpenGLWidget::grabFramebuffer (offscreen-friendly), then quit. Lets a script
  // capture each tonemap/env/key variant without a Wayland screenshot tool.
  if (opts.benchmark) {
    QTimer::singleShot(opts.delay_ms, view, [view, opts] {
      // Save one framed frame so the operator can confirm the robot fills the
      // view (coverage drives fragment cost) before trusting the numbers.
      const QImage img = view->grabFramebuffer();
      if (!img.isNull()) {
        const QString path = QStringLiteral("/tmp/bench_first_frame.png");
        img.save(path);
        std::printf("[mesh_viewer] framing preview: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
      }
      runBenchmark(view, opts);
    });
  } else if (!opts.screenshot_path.isEmpty()) {
    QTimer::singleShot(opts.delay_ms, view, [view, path = opts.screenshot_path] {
      const QImage img = view->grabFramebuffer();
      if (!img.isNull() && img.save(path)) {
        std::printf("[mesh_viewer] screenshot saved: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
      } else {
        std::fprintf(stderr, "[mesh_viewer] screenshot FAILED: %s\n", qPrintable(path));
      }
      QApplication::quit();
    });
  }
  return QApplication::exec();
}
