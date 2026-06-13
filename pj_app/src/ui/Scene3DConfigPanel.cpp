// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/Scene3DConfigPanel.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/layers/robot_model_layer.h"
#include "pj_scene3d_widgets/mesh_shading_params.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/ConfigPanelHost.h"
#include "pj_widgets/Dialog.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {

constexpr char kUrdfBrowseDirKey[] = "pj_scene3d/urdf_browse_dir";
constexpr auto kVisibilityOnPath = ":/resources/svg/visibility.svg";
constexpr auto kVisibilityOffPath = ":/resources/svg/visibility_off.svg";
constexpr auto kTrashIconPath = ":/resources/svg/trash.svg";
constexpr auto kAddIconPath = ":/resources/svg/add_circle.svg";

// Small modal prompt on the shared Dialog chrome: a single field + OK/Cancel.
// The field is parented into the dialog; values must be read before `dialog`
// leaves scope, which is why each picker below returns the value, not a bool.
bool execFieldDialog(Dialog& dialog, const QString& title, QWidget* field) {
  dialog.setDialogTitle(title);
  auto* layout = new QVBoxLayout(dialog.contentWidget());
  layout->addWidget(field);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.contentWidget());
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  return dialog.exec() == QDialog::Accepted;
}

// Topic mode: pick one of the dataset's robot_description topics.
std::optional<std::pair<ObjectTopicId, QString>> pickRobotDescriptionTopic(
    QWidget* parent, const QList<Scene3DDockWidget::RobotDescriptionTopic>& topics) {
  Dialog dialog(parent);
  auto* combo = new QComboBox(dialog.contentWidget());
  for (const auto& topic : topics) {
    combo->addItem(topic.name, QVariant::fromValue(static_cast<uint>(topic.topic_id.id)));
  }
  if (!execFieldDialog(dialog, QObject::tr("Robot description topic"), combo) || combo->currentIndex() < 0) {
    return std::nullopt;
  }
  ObjectTopicId topic_id;
  topic_id.id = combo->currentData().toUInt();
  return std::make_pair(topic_id, combo->currentText());
}

// URL mode: free-text http(s) URDF location.
std::optional<QString> promptUrdfUrl(QWidget* parent) {
  Dialog dialog(parent);
  auto* edit = new QLineEdit(dialog.contentWidget());
  edit->setPlaceholderText(QStringLiteral("https://example.com/robot.urdf"));
  edit->setMinimumWidth(360);
  if (!execFieldDialog(dialog, QObject::tr("Load URDF from URL"), edit)) {
    return std::nullopt;
  }
  const QString url = edit->text().trimmed();
  return url.isEmpty() ? std::nullopt : std::optional<QString>(url);
}

[[nodiscard]] ObjectTopicId topicFromRowId(qint64 id) {
  ObjectTopicId topic_id;
  topic_id.id = static_cast<uint32_t>(id);
  return topic_id;
}

[[nodiscard]] LayerRow rowFromLayerInfo(const SceneLayerInfo& info) {
  return LayerRow{
      .id = static_cast<qint64>(info.topic_id.id),
      .name = info.display_name,
      .visible = info.visible,
  };
}

DoubleScrubber* makeScrubber(double min, double max, double step, double value) {
  auto* scrubber = new DoubleScrubber;
  scrubber->setRange(min, max);
  scrubber->setSingleStep(step);
  scrubber->setDecimals(2);
  scrubber->setValue(value);
  return scrubber;
}

}  // namespace

Scene3DConfigPanel::Scene3DConfigPanel(QWidget* parent) : QWidget(parent) {
  // Scene-control persistence is debounced via settings_writer_ (apply stays
  // live per tick; the QSettings write coalesces to once the drag settles).

  // Zero outer margins so the section header bands (Grid · Transforms and
  // RobotModel · Topics · Settings) span edge-to-edge like the plotting
  // panel's Curve Width / Curve Style bands; each content block under a band
  // re-adds its own 8-px horizontal inset.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  buildSceneControls(outer);

  outer->addWidget(new SectionHeaderBand(tr("Topics"), this));
  auto* topics_host = new QWidget(this);
  auto* topics_layout = new QVBoxLayout(topics_host);
  topics_layout->setContentsMargins(8, 4, 8, 4);
  layer_list_ = new LayerListView(topics_host);
  topics_layout->addWidget(layer_list_);
  outer->addWidget(topics_host);

  outer->addWidget(new SectionHeaderBand(tr("Settings"), this));
  auto* settings_host = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings_host);
  settings_layout->setContentsMargins(8, 4, 8, 4);
  config_host_ = new ConfigPanelHost(settings_host);
  settings_layout->addWidget(config_host_);
  settings_layout->addStretch(1);
  outer->addWidget(settings_host, /*stretch=*/1);

  connect(layer_list_, &LayerListView::selectionChanged, this, &Scene3DConfigPanel::onLayerSelectionChanged);
  connect(layer_list_, &LayerListView::visibilityToggled, this, [this](qint64 id, bool visible) {
    if (bound_dock_ != nullptr) {
      bound_dock_->setLayerVisible(topicFromRowId(id), visible);
    }
  });
  connect(layer_list_, &LayerListView::removeRequested, this, [this](qint64 id) {
    if (bound_dock_ != nullptr) {
      bound_dock_->removeTopic(topicFromRowId(id));
    }
  });
  connect(layer_list_, &LayerListView::reordered, this, [this](const std::vector<qint64>& ids) {
    if (bound_dock_ != nullptr) {
      bound_dock_->reorderLayers(topicOrderFromIds(ids));
    }
  });

  applyIcons();
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::buildSceneControls(QVBoxLayout* root) {
  QSettings settings;
  settings.beginGroup(QString::fromLatin1(kScene3dSceneControlsGroup));
  // Each control: init from QSettings (defaults = the User's look-dev pick),
  // persist + re-apply to the bound dock on every change.
  const auto wire = [this, &settings](auto* widget, const char* key, auto read, auto signal) {
    widget->setProperty("settings_key", QString::fromLatin1(key));
    if (const QVariant saved = settings.value(QString::fromLatin1(key)); saved.isValid()) {
      read(saved);
    }
    connect(widget, signal, this, [this, widget]() {
      // Persist debounced (one INI rewrite per settled drag, not per tick), but
      // apply live every tick so the view tracks the scrubber.
      const QString settings_key = widget->property("settings_key").toString();
      if (auto* dscrub = qobject_cast<DoubleScrubber*>(widget)) {
        settings_writer_.queue(settings_key, dscrub->value());
      } else if (auto* iscrub = qobject_cast<IntScrubber*>(widget)) {
        settings_writer_.queue(settings_key, iscrub->value());
      }
      applySceneControls();
    });
  };

  // Eye toggles: checked = visible. Persisted like the other controls; the
  // icon mirrors the checked state (visibility / visibility_off). The shared
  // curveVisibilityToggle objectName picks up the QSS rule that keeps these
  // flat in every state — no checked/hover wash, the glyph is the indicator.
  const auto make_eye = [this, &settings](const char* key, const QString& tip) {
    auto* eye = new QToolButton(this);
    eye->setObjectName(QStringLiteral("curveVisibilityToggle"));
    eye->setCheckable(true);
    eye->setAutoRaise(true);
    eye->setFocusPolicy(Qt::NoFocus);
    eye->setToolTip(tip);
    eye->setProperty("settings_key", QString::fromLatin1(key));
    eye->setChecked(settings.value(QString::fromLatin1(key), true).toBool());
    connect(eye, &QToolButton::toggled, this, [this, eye](bool checked) {
      settings_writer_.queue(eye->property("settings_key").toString(), checked);
      setEyeIcon(eye, checked);
      applySceneControls();
    });
    return eye;
  };
  // Field + eye on one form row, eye hugging the control like the mockup.
  const auto with_eye = [](QWidget* field, QToolButton* eye) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(field, 1);
    row->addWidget(eye);
    return row;
  };

  const auto add_band = [this, root](const QString& text) { root->addWidget(new SectionHeaderBand(text, this)); };
  const auto add_form = [this, root]() {
    auto* host = new QWidget(this);
    auto* form = new QFormLayout(host);
    form->setContentsMargins(8, 4, 8, 4);
    root->addWidget(host);
    return form;
  };

  // --- Grid ---------------------------------------------------------------
  add_band(tr("Grid"));
  QFormLayout* grid_form = add_form();

  auto* style_row = new QHBoxLayout;
  style_row->setContentsMargins(0, 0, 0, 0);
  const auto make_style_button = [this](const QString& tip) {
    auto* button = new QToolButton(this);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tip);
    return button;
  };
  grid_lines_button_ = make_style_button(tr("Line grid"));
  grid_cells_button_ = make_style_button(tr("Filled cells"));
  auto* style_group = new QButtonGroup(this);
  style_group->setExclusive(true);
  style_group->addButton(grid_lines_button_, 0);
  style_group->addButton(grid_cells_button_, 1);
  const int saved_style = settings.value(QStringLiteral("grid_style"), 0).toInt();
  (saved_style == 1 ? grid_cells_button_ : grid_lines_button_)->setChecked(true);
  connect(style_group, &QButtonGroup::idClicked, this, [this](int id) {
    settings_writer_.queue(QStringLiteral("grid_style"), id);
    applySceneControls();
  });
  grid_eye_ = make_eye("grid_visible", tr("Show/hide the grid"));
  style_row->addWidget(grid_lines_button_);
  style_row->addWidget(grid_cells_button_);
  style_row->addWidget(grid_eye_);
  style_row->addStretch(1);
  grid_form->addRow(tr("Style"), style_row);

  grid_size_ = makeScrubber(1.0, 1000.0, 1.0, 10.0);
  grid_size_->setDecimals(0);
  wire(
      grid_size_, "grid_size", [this](const QVariant& v) { grid_size_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  grid_form->addRow(tr("Size (m)"), grid_size_);

  grid_divisions_ = new IntScrubber;
  grid_divisions_->setRange(1, 200);
  grid_divisions_->setValue(10);
  wire(
      grid_divisions_, "grid_divisions", [this](const QVariant& v) { grid_divisions_->setValue(v.toInt()); },
      qOverload<int>(&IntScrubber::valueChanged));
  grid_form->addRow(tr("Divisions"), grid_divisions_);

  // --- Transforms and RobotModel --------------------------------------------
  add_band(tr("Transforms and RobotModel"));
  QFormLayout* tm_form = add_form();

  // "Frames" in the UI = the TF frame axis triads (gizmo_* internally and in
  // the persisted settings keys, kept for compatibility).
  gizmo_size_ = makeScrubber(0.01, 5.0, 0.05, 0.15);
  wire(
      gizmo_size_, "gizmo_size", [this](const QVariant& v) { gizmo_size_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  tm_form->addRow(tr("Frames size (m)"), gizmo_size_);

  gizmo_opacity_ = makeScrubber(0.0, 1.0, 0.1, 1.0);
  gizmo_eye_ = make_eye("gizmos_visible", tr("Show/hide the TF frames"));
  wire(
      gizmo_opacity_, "gizmo_opacity", [this](const QVariant& v) { gizmo_opacity_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  tm_form->addRow(tr("Frames opacity"), with_eye(gizmo_opacity_, gizmo_eye_));

  model_source_combo_ = new QComboBox;
  model_source_combo_->addItem(tr("File"));
  model_source_combo_->addItem(tr("Topic"));
  model_source_combo_->addItem(tr("URL"));
  add_model_button_ = new QToolButton(this);
  add_model_button_->setAutoRaise(true);
  add_model_button_->setFocusPolicy(Qt::NoFocus);
  add_model_button_->setToolTip(tr("Add a robot model from the selected source"));
  auto* model_row = new QHBoxLayout;
  model_row->setContentsMargins(0, 0, 0, 0);
  model_row->addWidget(model_source_combo_, 1);
  model_row->addWidget(add_model_button_);
  tm_form->addRow(tr("Model/URDF"), model_row);
  connect(add_model_button_, &QToolButton::clicked, this, &Scene3DConfigPanel::onAddModelClicked);

  // One row per panel-added robot model (name + bin), appended below the
  // Model/URDF row by addRobotRow.
  robot_rows_layout_ = new QVBoxLayout;
  robot_rows_layout_->setContentsMargins(0, 0, 0, 0);
  robot_rows_layout_->setSpacing(2);
  tm_form->addRow(robot_rows_layout_);

  mesh_opacity_ = makeScrubber(0.0, 1.0, 0.1, 1.0);
  mesh_eye_ = make_eye("meshes_visible", tr("Show/hide visual meshes"));
  wire(
      mesh_opacity_, "mesh_opacity", [this](const QVariant& v) { mesh_opacity_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  tm_form->addRow(tr("Meshes opacity"), with_eye(mesh_opacity_, mesh_eye_));

  collision_opacity_ = makeScrubber(0.0, 1.0, 0.1, 0.4);
  collision_eye_ = make_eye("collisions_visible", tr("Show/hide collision meshes"));
  wire(
      collision_opacity_, "collision_opacity",
      [this](const QVariant& v) { collision_opacity_->setValue(v.toDouble()); },
      qOverload<double>(&DoubleScrubber::valueChanged));
  tm_form->addRow(tr("Collision opacity"), with_eye(collision_opacity_, collision_eye_));
}

void Scene3DConfigPanel::applySceneControls() {
  applySceneControlsTo(bound_dock_.data());
}

void Scene3DConfigPanel::applySceneControlsTo(Scene3DDockWidget* dock) {
  if (dock == nullptr) {
    return;
  }
  auto* view = dock->sceneView();
  if (view == nullptr) {
    return;
  }
  // The scene-control field set is mirrored across FOUR sites — keep them in sync
  // when adding a control (a miss is silent, no compile error): this push,
  // loadControlsFromDock (reflect-on-bind), and Scene3DDockWidget's
  // xmlSaveState/xmlLoadState (per-dock layout persistence).
  view->setGridVisible(grid_eye_->isChecked());
  view->setGridStyle(
      grid_cells_button_->isChecked() ? pj::scene3d::GridRenderPass::Style::kFilledCells
                                      : pj::scene3d::GridRenderPass::Style::kLines);
  view->setGridExtentMetres(static_cast<float>(grid_size_->value()));
  view->setGridDivisions(grid_divisions_->value());
  view->setAxesVisible(gizmo_eye_->isChecked());
  view->setGizmoSize(static_cast<float>(gizmo_size_->value()));
  view->setGizmoOpacity(static_cast<float>(gizmo_opacity_->value()));

  // Per-view look knobs: drives only the bound dock's view. Sibling docks keep
  // their own MeshShadingParams and converge when the panel rebinds and applies.
  auto& shading = view->meshShadingParams();
  shading.meshes_visible = mesh_eye_->isChecked();
  shading.mesh_opacity = static_cast<float>(mesh_opacity_->value());
  shading.collisions_visible = collision_eye_->isChecked();
  shading.collision_opacity = static_cast<float>(collision_opacity_->value());
  view->update();
}

void Scene3DConfigPanel::loadControlsFromDock(Scene3DDockWidget* dock) {
  if (dock == nullptr) {
    return;
  }
  auto* view = dock->sceneView();
  if (view == nullptr) {
    return;
  }
  // Inverse of applySceneControlsTo (keep the field set in sync — 4 sites).
  // Reflect the dock's OWN look in the widgets without echoing it straight back
  // through the change handlers (which would re-apply + re-persist). Scrubbers
  // are blocked; the eye toggles are set + their icon refreshed by hand (the
  // toggled handler that normally swaps the icon is suppressed by the blocker).
  const auto set_eye = [this](QToolButton* eye, bool on) {
    const QSignalBlocker block(eye);
    eye->setChecked(on);
    setEyeIcon(eye, on);
  };

  {
    const QSignalBlocker b_grid_size(grid_size_);
    const QSignalBlocker b_grid_div(grid_divisions_);
    const QSignalBlocker b_gizmo_size(gizmo_size_);
    const QSignalBlocker b_gizmo_op(gizmo_opacity_);
    const QSignalBlocker b_mesh_op(mesh_opacity_);
    const QSignalBlocker b_coll_op(collision_opacity_);

    grid_size_->setValue(view->gridExtentMetres());
    grid_divisions_->setValue(view->gridDivisions());
    gizmo_size_->setValue(view->gizmoSize());
    gizmo_opacity_->setValue(view->gizmoOpacity());

    const auto& shading = view->meshShadingParams();
    mesh_opacity_->setValue(shading.mesh_opacity);
    collision_opacity_->setValue(shading.collision_opacity);
    set_eye(mesh_eye_, shading.meshes_visible);
    set_eye(collision_eye_, shading.collisions_visible);
  }

  // idClicked (the connected signal) fires only on user clicks, not programmatic
  // setChecked, so the exclusive style group needs no blocker.
  (view->gridStyle() == pj::scene3d::GridRenderPass::Style::kFilledCells ? grid_cells_button_ : grid_lines_button_)
      ->setChecked(true);
  set_eye(grid_eye_, view->gridVisible());
  set_eye(gizmo_eye_, view->axesVisible());
}

void Scene3DConfigPanel::setEyeIcon(QToolButton* eye, bool on) {
  eye->setIcon(LoadSvg(QLatin1String(on ? kVisibilityOnPath : kVisibilityOffPath), theme_));
}

void Scene3DConfigPanel::applyIcons() {
  if (grid_lines_button_ != nullptr) {
    grid_lines_button_->setIcon(LoadSvg(QStringLiteral(":/resources/svg/grid_4x4.svg"), theme_));
  }
  if (grid_cells_button_ != nullptr) {
    grid_cells_button_->setIcon(LoadSvg(QStringLiteral(":/resources/svg/grid_view.svg"), theme_));
  }
  for (QToolButton* eye : {grid_eye_, gizmo_eye_, mesh_eye_, collision_eye_}) {
    if (eye != nullptr) {
      setEyeIcon(eye, eye->isChecked());
    }
  }
  if (add_model_button_ != nullptr) {
    add_model_button_->setIcon(LoadSvg(QLatin1String(kAddIconPath), theme_));
  }
  for (const auto& [id, row] : robot_rows_) {
    if (auto* trash = row->findChild<QToolButton*>()) {
      trash->setIcon(LoadSvg(QLatin1String(kTrashIconPath), theme_));
    }
  }
}

void Scene3DConfigPanel::onAddModelClicked() {
  if (bound_dock_ == nullptr) {
    return;
  }
  switch (model_source_combo_->currentIndex()) {
    case 0: {  // File
      QSettings settings;
      const QString start_dir = settings.value(QString::fromLatin1(kUrdfBrowseDirKey)).toString();
      const QString path = QFileDialog::getOpenFileName(
          this, tr("Load URDF"), start_dir, tr("URDF files (*.urdf *.xml);;All files (*)"));
      if (path.isEmpty()) {
        return;
      }
      if (bound_dock_ == nullptr) {
        return;  // the modal event loop can outlive the dock
      }
      settings.setValue(QString::fromLatin1(kUrdfBrowseDirKey), QFileInfo(path).absolutePath());
      const uint32_t id = bound_dock_->addRobotModelLayer(path).id;
      if (id != 0) {
        addRobotRow(id, QFileInfo(path).fileName(), path);
      }
      break;
    }
    case 1: {  // Topic
      const auto topics = bound_dock_->robotDescriptionTopics();
      if (topics.isEmpty()) {
        MessageBox::information(this, tr("Load robot model"), tr("No robot description topic in this dataset."));
        return;
      }
      const auto picked = pickRobotDescriptionTopic(this, topics);
      if (!picked.has_value()) {
        return;
      }
      if (bound_dock_ == nullptr) {
        return;  // the modal event loop can outlive the dock
      }
      // addRobotRow is idempotent, so the row created here is harmless when the
      // layerAdded signal (or a pre-existing layer) would have added it anyway.
      if (bound_dock_->addTopic(picked->first, sdk::BuiltinObjectType::kRobotDescription, picked->second)) {
        addRobotRow(picked->first.id, picked->second, picked->second);
      }
      break;
    }
    case 2: {  // URL
      const auto url = promptUrdfUrl(this);
      if (!url.has_value()) {
        return;
      }
      if (bound_dock_ == nullptr) {
        return;  // the modal event loop can outlive the dock
      }
      const uint32_t id = bound_dock_->addRobotModelLayerFromUrl(*url).id;
      if (id != 0) {
        const QString file_name = QUrl(*url).fileName();
        addRobotRow(id, file_name.isEmpty() ? *url : file_name, *url);
      }
      break;
    }
    default:
      break;
  }
}

void Scene3DConfigPanel::addRobotRow(uint32_t topic_id_value, const QString& label, const QString& tooltip) {
  // Idempotent: rows are derived from dock state and rebuilt on every bind, and
  // both onAddModelClicked and the layerAdded signal can target the same id.
  const auto existing = std::find_if(
      robot_rows_.begin(), robot_rows_.end(), [&](const auto& pair) { return pair.first == topic_id_value; });
  if (existing != robot_rows_.end()) {
    return;
  }

  auto* row = new QWidget(this);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(2);
  auto* name = new QLineEdit(label, row);
  name->setReadOnly(true);
  name->setFocusPolicy(Qt::NoFocus);
  name->setAlignment(Qt::AlignCenter);
  name->setToolTip(tooltip);
  // Clicking the name field selects the robot: it binds the Settings host to the
  // layer's config widget (source combo / status / Retry). The id rides on a
  // property so the panel's eventFilter can route the press without per-row state.
  name->setProperty("robot_topic_id", topic_id_value);
  name->setCursor(Qt::PointingHandCursor);
  name->installEventFilter(this);
  layout->addWidget(name, 1);
  auto* trash = new QToolButton(row);
  // Same flat styling as the topic-row trash buttons (QSS keys on this name).
  trash->setObjectName(QStringLiteral("curveTrashToggle"));
  trash->setAutoRaise(true);
  trash->setFocusPolicy(Qt::NoFocus);
  trash->setToolTip(tr("Remove this robot model"));
  trash->setIcon(LoadSvg(QLatin1String(kTrashIconPath), theme_));
  layout->addWidget(trash);
  connect(trash, &QToolButton::clicked, this, [this, topic_id_value]() {
    if (bound_dock_ != nullptr) {
      ObjectTopicId topic_id;
      topic_id.id = topic_id_value;
      bound_dock_->removeTopic(topic_id);  // the row is dropped by onLayerRemoved
    }
  });
  robot_rows_layout_->addWidget(row);
  robot_rows_.emplace_back(topic_id_value, row);

  // Mirror the layer's status onto the name tooltip — the cheap error surface
  // for load failures ("Fetch failed …" / "Failed to read URDF …") that would
  // otherwise be invisible until the row is clicked. Kept fresh via the signal.
  if (bound_dock_ != nullptr) {
    ObjectTopicId topic_id;
    topic_id.id = topic_id_value;
    if (auto* robot = qobject_cast<pj::scene3d::RobotModelLayer*>(bound_dock_->layerFor(topic_id))) {
      const QString status = robot->statusText();
      if (!status.isEmpty()) {
        name->setToolTip(status);
      }
      connect(
          robot, &pj::scene3d::RobotModelLayer::statusTextChanged, name, [name, tooltip](const QString& status_text) {
            name->setToolTip(status_text.isEmpty() ? tooltip : status_text);
          });
    }
  }
}

void Scene3DConfigPanel::showRobotLayerConfig(uint32_t topic_id_value) {
  if (bound_dock_ == nullptr) {
    return;
  }
  ObjectTopicId topic_id;
  topic_id.id = topic_id_value;
  ISceneLayer* layer = bound_dock_->layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  // Robot layers aren't in the Topics list, so the list selection is unrelated;
  // last click wins on config_host_ (the list selection stays as-is).
  config_host_->setConfigWidget(layer->createConfigWidget(config_host_));
}

bool Scene3DConfigPanel::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() == Qt::LeftButton) {
      const QVariant topic_id = watched->property("robot_topic_id");
      if (topic_id.isValid()) {
        showRobotLayerConfig(topic_id.toUInt());
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void Scene3DConfigPanel::removeRobotRowFor(uint32_t topic_id_value) {
  const auto it = std::find_if(
      robot_rows_.begin(), robot_rows_.end(), [&](const auto& pair) { return pair.first == topic_id_value; });
  if (it == robot_rows_.end()) {
    return;
  }
  it->second->deleteLater();
  robot_rows_.erase(it);
}

void Scene3DConfigPanel::bindDock(Scene3DDockWidget* dock) {
  if (bound_dock_.data() == dock) {
    return;
  }
  disconnectFromDock();
  bound_dock_ = dock;

  layer_list_->clearRows();
  config_host_->clear();
  // Robot rows belong to the dock they were added to; a rebind starts from a
  // clean slate (the previous dock keeps its layers).
  for (const auto& [id, row] : robot_rows_) {
    row->deleteLater();
  }
  robot_rows_.clear();

  if (dock == nullptr) {
    updateSelectedLayerPane();
    return;
  }

  rebuildLayerList();
  loadControlsFromDock(dock);  // reflect THIS dock's look; controls are per-dock

  connect(dock, &SceneDockWidget::layerAdded, this, &Scene3DConfigPanel::onLayerAdded);
  connect(dock, &SceneDockWidget::layerRemoved, this, &Scene3DConfigPanel::onLayerRemoved);
  connect(dock, &SceneDockWidget::layerVisibilityChanged, this, &Scene3DConfigPanel::onLayerVisibilityChanged);
  connect(dock, &SceneDockWidget::layerWarningChanged, this, &Scene3DConfigPanel::onLayerWarningChanged);
}

void Scene3DConfigPanel::disconnectFromDock() {
  if (bound_dock_ != nullptr) {
    disconnect(bound_dock_.data(), nullptr, this, nullptr);
  }
  bound_dock_ = nullptr;
}

void Scene3DConfigPanel::rebuildLayerList() {
  if (bound_dock_ == nullptr) {
    layer_list_->clearRows();
    updateSelectedLayerPane();
    return;
  }

  std::vector<LayerRow> rows;
  const auto layers = bound_dock_->layers();
  rows.reserve(layers.size());
  for (const SceneLayerInfo& info : layers) {
    // Robot-model layers stay out of the Topics list; they get a dedicated row
    // under the Model/URDF selector, rebuilt here from dock state (so layout
    // restore and dock switches recover the rows — bindDock cleared them).
    if (info.object_type == sdk::BuiltinObjectType::kRobotDescription) {
      addRobotRow(info.topic_id.id, info.display_name, info.display_name);
      continue;
    }
    rows.push_back(rowFromLayerInfo(info));
  }
  layer_list_->setRows(rows);

  for (const LayerRow& row : rows) {
    const auto warning = bound_dock_->orphanState(topicFromRowId(row.id));
    layer_list_->setRowWarning(row.id, warning.is_orphan, warning.reason);
  }
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerSelectionChanged() {
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerAdded(ObjectTopicId topic_id) {
  if (bound_dock_ == nullptr) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  const SceneLayerInfo info = layer->info();
  if (info.object_type == sdk::BuiltinObjectType::kRobotDescription) {
    // Robot layers get a Model/URDF row, not a Topics-list row. Idempotent, so
    // a row already created by onAddModelClicked is not duplicated here.
    addRobotRow(info.topic_id.id, info.display_name, info.display_name);
    return;
  }
  layer_list_->addRow(rowFromLayerInfo(info));
  const auto warning = bound_dock_->orphanState(topic_id);
  layer_list_->setRowWarning(static_cast<qint64>(topic_id.id), warning.is_orphan, warning.reason);
}

void Scene3DConfigPanel::onLayerRemoved(ObjectTopicId topic_id) {
  layer_list_->removeRow(static_cast<qint64>(topic_id.id));
  // Single removal path for robot rows: the bin button only calls removeTopic
  // and this signal drops the row, so other teardown paths stay consistent.
  removeRobotRowFor(topic_id.id);
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerVisibilityChanged(ObjectTopicId topic_id, bool visible) {
  layer_list_->setRowVisible(static_cast<qint64>(topic_id.id), visible);
}

void Scene3DConfigPanel::onLayerWarningChanged(ObjectTopicId topic_id, bool warn, const QString& reason) {
  layer_list_->setRowWarning(static_cast<qint64>(topic_id.id), warn, reason);
}

void Scene3DConfigPanel::onStylesheetChanged(QString theme) {
  theme_ = theme;
  layer_list_->setTheme(std::move(theme));
  applyIcons();
}

void Scene3DConfigPanel::updateSelectedLayerPane() {
  config_host_->clear();
  if (bound_dock_ == nullptr) {
    return;
  }
  const auto selected = selectedTopicId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(*selected);
  if (layer == nullptr) {
    return;
  }
  config_host_->setConfigWidget(layer->createConfigWidget(config_host_));
}

std::optional<ObjectTopicId> Scene3DConfigPanel::selectedTopicId() const {
  if (layer_list_ == nullptr) {
    return std::nullopt;
  }
  const auto id = layer_list_->currentId();
  if (!id.has_value()) {
    return std::nullopt;
  }
  return topicFromRowId(*id);
}

std::vector<ObjectTopicId> Scene3DConfigPanel::topicOrderFromIds(const std::vector<qint64>& ids) const {
  std::vector<ObjectTopicId> ordered;
  ordered.reserve(ids.size());
  for (const qint64 id : ids) {
    ordered.push_back(topicFromRowId(id));
  }
  return ordered;
}

}  // namespace PJ
