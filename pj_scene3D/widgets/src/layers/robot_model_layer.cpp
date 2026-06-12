// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/robot_model_layer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDomElement>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <any>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "mesh_loader.h"
#include "pj_base/builtin/robot_description.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "pj_widgets/SvgUtil.h"
#include "urdf_package_resolver.h"
#include "urdf_parser.h"

namespace pj::scene3d {
namespace {
Q_LOGGING_CATEGORY(lcRobotModelLayer, "pj.scene3d.layer.robot_model")

constexpr auto kLatchRetryInterval = std::chrono::milliseconds(500);

class ColorSwatch : public QPushButton {
 public:
  explicit ColorSwatch(QColor color, QWidget* parent = nullptr) : QPushButton(parent), color_(std::move(color)) {
    setCursor(Qt::PointingHandCursor);
    setFlat(true);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(22, 22);
  }

  void setColor(QColor color) {
    color_ = std::move(color);
    update();
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color_);
    const QRectF rect(3.0, 3.0, width() - 6.0, height() - 6.0);
    painter.drawRoundedRect(rect, 3.0, 3.0);
  }

 private:
  QColor color_;
};

QString sourceTypeToString(RobotModelLayer::SourceType type) {
  switch (type) {
    case RobotModelLayer::SourceType::kFile:
      return QStringLiteral("file");
    case RobotModelLayer::SourceType::kUrl:
      return QStringLiteral("url");
    case RobotModelLayer::SourceType::kTopic:
    default:
      return QStringLiteral("topic");
  }
}

RobotModelLayer::SourceType sourceTypeFromString(const QString& s) {
  if (s == QStringLiteral("file")) {
    return RobotModelLayer::SourceType::kFile;
  }
  if (s == QStringLiteral("url")) {
    return RobotModelLayer::SourceType::kUrl;
  }
  return RobotModelLayer::SourceType::kTopic;
}

QString displayModeToString(RobotModelLayer::DisplayMode mode) {
  switch (mode) {
    case RobotModelLayer::DisplayMode::kVisual:
      return QStringLiteral("visual");
    case RobotModelLayer::DisplayMode::kCollision:
      return QStringLiteral("collision");
    case RobotModelLayer::DisplayMode::kAuto:
    default:
      return QStringLiteral("auto");
  }
}

RobotModelLayer::DisplayMode displayModeFromString(const QString& s) {
  if (s == QStringLiteral("visual")) {
    return RobotModelLayer::DisplayMode::kVisual;
  }
  if (s == QStringLiteral("collision")) {
    return RobotModelLayer::DisplayMode::kCollision;
  }
  return RobotModelLayer::DisplayMode::kAuto;
}

PJ::sdk::BuiltinObjectType objectTypeFromMetadata(const std::string& metadata_json) {
  if (metadata_json.empty()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(metadata_json));
  if (!doc.isObject()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const QJsonValue value = doc.object().value(QStringLiteral("builtin_object_type"));
  if (!value.isString()) {
    return PJ::sdk::BuiltinObjectType::kNone;
  }
  const auto parsed = PJ::sdk::parseBuiltinObjectType(value.toString().toStdString());
  return parsed.value_or(PJ::sdk::BuiltinObjectType::kNone);
}

void addObjectTopicToCombo(QComboBox* combo, PJ::ObjectTopicId topic_id, const PJ::ObjectTopicDescriptor& desc) {
  combo->addItem(QString::fromStdString(desc.topic_name), QVariant::fromValue(static_cast<uint>(topic_id.id)));
}

std::optional<PJ::ObjectTopicId> currentObjectTopicId(const QComboBox* combo) {
  if (combo == nullptr || !combo->isEnabled() || combo->currentIndex() < 0) {
    return std::nullopt;
  }
  bool ok = false;
  const uint id = combo->currentData().toUInt(&ok);
  if (!ok || id == 0) {
    return std::nullopt;
  }
  return PJ::ObjectTopicId{.id = static_cast<uint32_t>(id)};
}

QString formatFromXml(const QString& text) {
  QDomDocument doc;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  const auto result = doc.setContent(text.toUtf8());
  if (!result) {
    return QStringLiteral("unknown");
  }
#else
  if (!doc.setContent(text)) {
    return QStringLiteral("unknown");
  }
#endif
  const QString root = doc.documentElement().tagName();
  return root == QStringLiteral("robot") ? QStringLiteral("urdf") : root;
}

std::optional<QString> readTextFile(const QString& path, QString* error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    if (error != nullptr) {
      *error = file.errorString();
    }
    return std::nullopt;
  }
  return QString::fromUtf8(file.readAll());
}

std::optional<QString> readUrlBlocking(const QString& url_text, QString* error) {
  const QUrl url(url_text);
  if (url.isLocalFile()) {
    return readTextFile(url.toLocalFile(), error);
  }
  if (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) {
    if (error != nullptr) {
      *error = QObject::tr("unsupported URL scheme");
    }
    return std::nullopt;
  }

  QNetworkAccessManager manager;
  QNetworkReply* reply = manager.get(QNetworkRequest(url));
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeout.start(15000);
  loop.exec();

  if (timeout.isActive()) {
    timeout.stop();
  } else {
    reply->abort();
    reply->deleteLater();
    if (error != nullptr) {
      *error = QObject::tr("request timed out");
    }
    return std::nullopt;
  }

  if (reply->error() != QNetworkReply::NoError) {
    if (error != nullptr) {
      *error = reply->errorString();
    }
    reply->deleteLater();
    return std::nullopt;
  }
  const QString text = QString::fromUtf8(reply->readAll());
  reply->deleteLater();
  return text;
}

QString urlDirectory(const QString& url_text) {
  QUrl url(url_text);
  QString path = url.path();
  const qsizetype slash = path.lastIndexOf('/');
  if (slash >= 0) {
    path = path.left(slash + 1);
  }
  url.setPath(path);
  url.setQuery(QString());
  url.setFragment(QString());
  return url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
}

glm::vec3 toVec3(const glm::dvec3& v) {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

}  // namespace

struct RobotModelLayer::MeshLoadRecord {
  std::string key;
  QString path;
  QFuture<MeshData> future;
  // GUI-thread completion hook: triggers pollMeshLoads() so a finished load
  // requests the repaint that consumes it (the app paints on demand — render()
  // alone would never run). Owned by the record so clearing the records
  // (detach, model reload) destroys the watcher and severs the connection: a
  // stale completion can never fire on a dead record.
  std::unique_ptr<QFutureWatcher<MeshData>> watcher;
  bool consumed{false};
  bool failed{false};
};

RobotModelLayer::RobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent),
      topic_id_(topic_id),
      source_topic_id_(topic_id),
      display_name_(std::move(display_name)),
      source_value_(display_name_),
      owned_resolver_(std::make_unique<UrdfPackageResolver>()),
      mesh_loader_(std::make_unique<MeshLoader>()),
      mesh_pass_(std::make_unique<MeshRenderPass>()) {
  resolver_ = owned_resolver_.get();
}

RobotModelLayer::~RobotModelLayer() = default;

PJ::SceneLayerInfo RobotModelLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kRobotDescription,
      .display_name = display_name_.isEmpty() ? tr("Robot model") : display_name_,
      .family_name = QStringLiteral("RobotModel"),
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> RobotModelLayer::timeRange() const {
  return {PJ::Timepoint::max(), PJ::Timepoint::min()};
}

QStringList RobotModelLayer::fallbackFrames() const {
  QStringList out;
  if (!model_.has_value()) {
    return out;
  }
  for (const RobotLink& link : model_->links) {
    out.push_back(linkFrameName(link.name));
  }
  return out;
}

QString RobotModelLayer::sourceFrame() const {
  if (!model_.has_value() || model_->root_link.empty()) {
    return {};
  }
  return linkFrameName(model_->root_link);
}

QDomElement RobotModelLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(QStringLiteral("robot_model"));
  el.setAttribute(QStringLiteral("source_type"), sourceTypeToString(source_type_));
  el.setAttribute(QStringLiteral("source_value"), source_value_);
  el.setAttribute(QStringLiteral("frame_prefix"), frame_prefix_);
  el.setAttribute(QStringLiteral("display_mode"), displayModeToString(display_mode_));
  el.setAttribute(QStringLiteral("visible"), visible_ ? QStringLiteral("true") : QStringLiteral("false"));
  el.setAttribute(QStringLiteral("color"), fallback_color_.name(QColor::HexRgb));
  el.setAttribute(
      QStringLiteral("ignore_collada_up_axis"),
      ignore_collada_up_axis_ ? QStringLiteral("true") : QStringLiteral("false"));
  return el;
}

bool RobotModelLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("robot_model")) {
    return false;
  }
  source_type_ = sourceTypeFromString(element.attribute(QStringLiteral("source_type"), QStringLiteral("topic")));
  source_value_ = element.attribute(QStringLiteral("source_value"), source_value_);
  frame_prefix_ = element.attribute(QStringLiteral("frame_prefix"));
  display_mode_ = displayModeFromString(element.attribute(QStringLiteral("display_mode"), QStringLiteral("auto")));
  visible_ = element.attribute(QStringLiteral("visible"), QStringLiteral("true")) == QStringLiteral("true");
  if (element.hasAttribute(QStringLiteral("color"))) {
    const QColor color(element.attribute(QStringLiteral("color")));
    if (color.isValid()) {
      fallback_color_ = color;
    }
  }
  ignore_collada_up_axis_ =
      element.attribute(QStringLiteral("ignore_collada_up_axis"), QStringLiteral("false")) == QStringLiteral("true");
  if (ctx_.session != nullptr) {
    loadFromCurrentSource();
  }
  emit infoChanged();
  emit visibilityChanged(visible_);
  emit repaintRequested();
  return true;
}

bool RobotModelLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  ctx_ = scene3d_ctx;
  if (source_type_ == SourceType::kTopic) {
    if (ctx_.session == nullptr) {
      qCWarning(lcRobotModelLayer) << "attach: session is null";
      return false;
    }
    parser_ = ctx_.session->parserForObjectTopic(source_topic_id_);
    parser_mutex_ = ctx_.session->parserMutexForObjectTopic(source_topic_id_);
    if (parser_ == nullptr) {
      qCWarning(lcRobotModelLayer) << "attach: no parser for robot-description topic" << source_topic_id_.id;
      return false;
    }
    const auto& desc = ctx_.session->objectStore().descriptor(source_topic_id_);
    if (source_value_.isEmpty()) {
      source_value_ = QString::fromStdString(desc.topic_name);
    }
  }
  loadFromCurrentSource();
  return true;
}

void RobotModelLayer::detach() {
  model_.reset();
  mesh_loads_.clear();
  if (mesh_pass_) {
    mesh_pass_->clearMeshes();
  }
  parser_ = nullptr;
  parser_mutex_.reset();
  ctx_ = {};
}

void RobotModelLayer::setFixedFrame(const QString& frame) {
  fixed_frame_ = frame;
  emit repaintRequested();
}

void RobotModelLayer::setTrackerTime(PJ::Timepoint time) {
  tracker_time_ = time;
  if (source_type_ == SourceType::kTopic && latch_pending_) {
    const auto now = std::chrono::steady_clock::now();
    if (last_latch_retry_ == std::chrono::steady_clock::time_point{} ||
        now - last_latch_retry_ >= kLatchRetryInterval) {
      last_latch_retry_ = now;
      tryLoadTopicDescription();
    }
  }
  emit repaintRequested();
}

void RobotModelLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  emit visibilityChanged(visible);
  emit infoChanged();
  emit repaintRequested();
}

void RobotModelLayer::initializeGL() {
  if (mesh_pass_) {
    mesh_pass_->initializeGL();
  }
}

void RobotModelLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_ || !mesh_pass_ || !model_.has_value()) {
    return;
  }
  pollMeshLoads();

  std::vector<MeshRenderPass::DrawCall> visual_draws;
  std::vector<MeshRenderPass::DrawCall> pending_collision_draws;

  const glm::vec4 placeholder_color{1.0f, 0.0f, 1.0f, 1.0f};

  auto append_geom = [&](const LinkGeom& geom, const glm::mat4& link_model, bool collision) {
    MeshRenderPass::DrawCall draw;
    draw.model = link_model * glm::mat4(originToMat4(geom.origin_xyz, geom.origin_rpy));
    if (geom.has_color) {
      draw.use_vertex_color = false;
      draw.color = geom.color;
    } else {
      draw.use_vertex_color = true;
      draw.color = glm::vec4(1.0f);
    }

    if (const auto* box = std::get_if<GeomBox>(&geom.shape); box != nullptr) {
      draw.kind = MeshRenderPass::GeometryKind::kBox;
      draw.model = glm::scale(draw.model, toVec3(box->size));
      draw.use_vertex_color = false;
    } else if (const auto* cylinder = std::get_if<GeomCylinder>(&geom.shape); cylinder != nullptr) {
      draw.kind = MeshRenderPass::GeometryKind::kCylinder;
      draw.model = glm::scale(
          draw.model, glm::vec3(
                          static_cast<float>(cylinder->radius), static_cast<float>(cylinder->radius),
                          static_cast<float>(cylinder->length)));
      draw.use_vertex_color = false;
    } else if (const auto* sphere = std::get_if<GeomSphere>(&geom.shape); sphere != nullptr) {
      draw.kind = MeshRenderPass::GeometryKind::kSphere;
      draw.model = glm::scale(draw.model, glm::vec3(static_cast<float>(sphere->radius)));
      draw.use_vertex_color = false;
    } else if (const auto* mesh = std::get_if<GeomMesh>(&geom.shape); mesh != nullptr) {
      draw.model = glm::scale(draw.model, toVec3(mesh->scale));
      if (mesh->resolved && meshReady(mesh->resolved_path)) {
        draw.kind = MeshRenderPass::GeometryKind::kMesh;
        draw.mesh_key = mesh->resolved_path;
      } else {
        draw.kind = MeshRenderPass::GeometryKind::kPlaceholderCube;
        draw.color = placeholder_color;
        draw.use_vertex_color = false;
      }
    }

    if (collision) {
      pending_collision_draws.push_back(std::move(draw));
    } else {
      visual_draws.push_back(std::move(draw));
    }
  };

  for (const RobotLink& link : model_->links) {
    const auto tf = frame_ctx.lookup(linkFrameName(link.name).toStdString());
    if (!tf.has_value()) {
      continue;
    }
    const glm::mat4 link_model = glm::mat4(tf->matrix());

    if (display_mode_ == DisplayMode::kCollision) {
      for (const LinkGeom& geom : link.collisions) {
        append_geom(geom, link_model, true);
      }
    } else if (display_mode_ == DisplayMode::kVisual) {
      for (const LinkGeom& geom : link.visuals) {
        append_geom(geom, link_model, false);
      }
    } else {
      const auto& geoms = link.visuals.empty() ? link.collisions : link.visuals;
      for (const LinkGeom& geom : geoms) {
        append_geom(geom, link_model, false);
      }
    }
  }

  // Scene-wide opacities (Part C "Meshes"/"Collision" sliders); 0 hides the
  // group entirely. The per-layer DisplayMode stays the structural override.
  const MeshShadingParams& shading = meshShadingParams();
  if (shading.meshes_visible && shading.mesh_opacity > 0.0f) {
    mesh_pass_->renderVisuals(view_params, visual_draws, shading.mesh_opacity);
  }
  if (shading.collisions_visible && shading.collision_opacity > 0.0f) {
    mesh_pass_->renderCollisions(view_params, pending_collision_draws, shading.collision_opacity);
  }
}

void RobotModelLayer::releaseGL() {
  if (mesh_pass_) {
    mesh_pass_->releaseGL();
  }
}

std::optional<AABB> RobotModelLayer::worldBounds() const {
  return std::nullopt;
}

QWidget* RobotModelLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(6);

  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(6);
  outer->addLayout(form);

  auto* source_combo = new QComboBox(container);
  source_combo->addItem(tr("Topic"), static_cast<int>(SourceType::kTopic));
  source_combo->addItem(tr("File"), static_cast<int>(SourceType::kFile));
  source_combo->addItem(tr("URL"), static_cast<int>(SourceType::kUrl));
  if (const int idx = source_combo->findData(static_cast<int>(source_type_)); idx >= 0) {
    source_combo->setCurrentIndex(idx);
  }
  form->addRow(tr("Source"), source_combo);

  auto* topic_combo = new QComboBox(container);
  if (ctx_.session != nullptr) {
    PJ::ObjectStore& store = ctx_.session->objectStore();
    for (const PJ::ObjectTopicId topic_id : store.listTopics()) {
      const PJ::ObjectTopicDescriptor& desc = store.descriptor(topic_id);
      if (objectTypeFromMetadata(desc.metadata_json) == PJ::sdk::BuiltinObjectType::kRobotDescription) {
        addObjectTopicToCombo(topic_combo, topic_id, desc);
      }
    }
  }
  if (topic_combo->count() == 0) {
    topic_combo->addItem(tr("No robot description topic in this dataset"), QVariant::fromValue(0U));
    QFont italic = topic_combo->font();
    italic.setItalic(true);
    topic_combo->setItemData(0, italic, Qt::FontRole);
    topic_combo->setEnabled(false);
  } else {
    int idx = topic_combo->findData(QVariant::fromValue(static_cast<uint>(source_topic_id_.id)));
    if (idx < 0) {
      idx = topic_combo->findText(QStringLiteral("/robot_description"));
    }
    topic_combo->setCurrentIndex(idx >= 0 ? idx : 0);
  }
  form->addRow(QString(), topic_combo);

  auto* file_row = new QWidget(container);
  auto* file_layout = new QHBoxLayout(file_row);
  file_layout->setContentsMargins(0, 0, 0, 0);
  // Shows only the file name (full path lives in source_value_ / the tooltip); the
  // field is read-only — Browse is the way to change it.
  auto* file_edit =
      new QLineEdit(source_type_ == SourceType::kFile ? QFileInfo(source_value_).fileName() : QString(), file_row);
  file_edit->setReadOnly(true);
  file_edit->setToolTip(source_type_ == SourceType::kFile ? source_value_ : QString());
  // Themed icon buttons, matching the app's chrome (resources are registered
  // process-wide by pj_app; the LayerListView eye/trash rows are the pattern).
  const QString icon_theme = QGuiApplication::palette().color(QPalette::Window).valueF() < 0.5
                                 ? QStringLiteral("dark")
                                 : QStringLiteral("light");
  auto* browse_button = new QToolButton(file_row);
  browse_button->setAutoRaise(true);
  browse_button->setFocusPolicy(Qt::NoFocus);
  browse_button->setToolTip(tr("Browse for a URDF file"));
  browse_button->setIcon(PJ::LoadSvg(QStringLiteral(":/resources/svg/folder_open.svg"), icon_theme));
  file_layout->addWidget(file_edit, 1);
  file_layout->addWidget(browse_button);
  form->addRow(QString(), file_row);

  auto* url_row = new QWidget(container);
  auto* url_layout = new QHBoxLayout(url_row);
  url_layout->setContentsMargins(0, 0, 0, 0);
  auto* url_edit = new QLineEdit(source_type_ == SourceType::kUrl ? source_value_ : QString(), url_row);
  auto* load_button = new QToolButton(url_row);
  load_button->setAutoRaise(true);
  load_button->setFocusPolicy(Qt::NoFocus);
  load_button->setToolTip(tr("Fetch the URDF from this URL"));
  load_button->setIcon(PJ::LoadSvg(QStringLiteral(":/resources/svg/import.svg"), icon_theme));
  url_layout->addWidget(url_edit, 1);
  url_layout->addWidget(load_button);
  form->addRow(QString(), url_row);

  auto* status_row = new QWidget(container);
  auto* status_layout = new QHBoxLayout(status_row);
  status_layout->setContentsMargins(0, 0, 0, 0);
  status_layout->setSpacing(4);
  auto* status_label = new QLabel(status_text_, status_row);
  status_label->setWordWrap(true);
  status_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto* locate_button = new QToolButton(status_row);
  locate_button->setText(tr("Locate..."));
  locate_button->setAutoRaise(true);
  auto* retry_button = new QToolButton(status_row);
  retry_button->setText(tr("Retry"));
  retry_button->setAutoRaise(true);
  status_layout->addWidget(status_label, 1);
  status_layout->addWidget(locate_button);
  status_layout->addWidget(retry_button);
  form->addRow(tr("Status"), status_row);

  auto* prefix_edit = new QLineEdit(frame_prefix_, container);
  prefix_edit->setPlaceholderText(tr("e.g. robot1/"));
  form->addRow(tr("Frame prefix"), prefix_edit);

  auto* mode_combo = new QComboBox(container);
  mode_combo->addItem(tr("Auto"), static_cast<int>(DisplayMode::kAuto));
  mode_combo->addItem(tr("Visual"), static_cast<int>(DisplayMode::kVisual));
  mode_combo->addItem(tr("Collision"), static_cast<int>(DisplayMode::kCollision));
  mode_combo->setCurrentIndex(static_cast<int>(display_mode_));
  form->addRow(tr("Display mode"), mode_combo);

  auto* color_button = new ColorSwatch(fallback_color_, container);
  form->addRow(tr("Color"), color_button);

  auto* collada_box = new QCheckBox(container);
  collada_box->setChecked(ignore_collada_up_axis_);
  form->addRow(tr("Ignore COLLADA up_axis"), collada_box);

  auto* group = new QGroupBox(tr("Mesh resolution"), container);
  group->setCheckable(true);
  group->setChecked(false);
  auto* group_layout = new QVBoxLayout(group);
  auto* roots_list = new QListWidget(group);
  if (resolver_ != nullptr) {
    roots_list->addItems(resolver_->searchRoots());
  }
  group_layout->addWidget(roots_list);
  outer->addWidget(group);

  const auto refresh_roots_list = [this, roots_list]() {
    roots_list->clear();
    if (resolver_ != nullptr) {
      roots_list->addItems(resolver_->searchRoots());
    }
  };
  const auto apply_source_visibility = [this, topic_combo, file_row, url_row]() {
    topic_combo->setVisible(source_type_ == SourceType::kTopic);
    file_row->setVisible(source_type_ == SourceType::kFile);
    url_row->setVisible(source_type_ == SourceType::kUrl);
  };
  const auto refresh_status = [this, status_label, locate_button, retry_button]() {
    QString status = status_text_;
    if (total_mesh_count_ > 0) {
      const int resolved = total_mesh_count_ - unresolved_mesh_count_;
      const qsizetype split = status.indexOf(QStringLiteral("  •  "));
      const QString prefix = split > 0 ? status.left(split) : tr("URDF: %1").arg(source_value_);
      if (unresolved_mesh_count_ > 0) {
        status = tr("%1  •  %2/%3 meshes  •  %4 packages unresolved")
                     .arg(prefix)
                     .arg(resolved)
                     .arg(total_mesh_count_)
                     .arg(unresolvedPackagesList().size());
      } else {
        status = tr("%1  •  %2/%2 meshes").arg(prefix).arg(total_mesh_count_);
      }
    }
    status_label->setText(status);
    locate_button->setVisible(!unresolvedPackagesList().isEmpty());
    retry_button->setVisible(!status_text_.isEmpty());
  };
  apply_source_visibility();
  refresh_status();

  connect(
      source_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, source_combo, topic_combo, file_edit, url_edit, apply_source_visibility, refresh_status](int) {
        const auto selected = static_cast<SourceType>(source_combo->currentData().toInt());
        source_type_ = selected;
        apply_source_visibility();
        if (selected == SourceType::kTopic) {
          if (const auto topic_id = currentObjectTopicId(topic_combo); topic_id.has_value()) {
            setSourceTopic(*topic_id, topic_combo->currentText());
          } else {
            setStatus(tr("No robot description topic in this dataset"));
          }
        } else if (selected == SourceType::kFile) {
          source_value_ = file_edit->text();
        } else {
          source_value_ = url_edit->text();
        }
        refresh_status();
      });
  connect(
      topic_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, topic_combo, refresh_status](int) {
        if (source_type_ != SourceType::kTopic) {
          return;
        }
        if (const auto topic_id = currentObjectTopicId(topic_combo); topic_id.has_value()) {
          setSourceTopic(*topic_id, topic_combo->currentText());
        }
        refresh_status();
      });
  connect(browse_button, &QToolButton::clicked, this, [this, container, file_edit, source_combo, refresh_status]() {
    // Remember the last-browsed folder in the app QSettings so reopening the
    // dialog lands where the user last picked a URDF (the current file wins if set).
    QSettings settings;
    const QString remembered = settings.value(QStringLiteral("pj_scene3d/urdf_browse_dir")).toString();
    const QString start_dir = !source_value_.isEmpty() ? source_value_ : remembered;
    const QString path = QFileDialog::getOpenFileName(
        container, tr("Open URDF"), start_dir, tr("URDF files (*.urdf *.xml);;All files (*)"));
    if (path.isEmpty()) {
      return;
    }
    settings.setValue(QStringLiteral("pj_scene3d/urdf_browse_dir"), QFileInfo(path).absolutePath());
    file_edit->setText(QFileInfo(path).fileName());
    file_edit->setToolTip(path);
    if (const int idx = source_combo->findData(static_cast<int>(SourceType::kFile)); idx >= 0) {
      source_combo->setCurrentIndex(idx);
    }
    setSourceFile(path);
    refresh_status();
  });
  connect(load_button, &QToolButton::clicked, this, [this, url_edit, source_combo, refresh_status]() {
    if (const int idx = source_combo->findData(static_cast<int>(SourceType::kUrl)); idx >= 0) {
      source_combo->setCurrentIndex(idx);
    }
    setSourceUrl(url_edit->text());
    refresh_status();
  });
  connect(locate_button, &QToolButton::clicked, this, [this, container, refresh_roots_list, refresh_status]() {
    const QStringList packages = unresolvedPackagesList();
    if (packages.isEmpty() || resolver_ == nullptr) {
      return;
    }
    const QString root = QFileDialog::getExistingDirectory(
        container, tr("Select the folder that contains your robot packages"), QString());
    if (root.isEmpty()) {
      return;
    }
    QStringList missing;
    for (const QString& package : packages) {
      const QString package_dir = QDir(root).filePath(package);
      if (QFileInfo(package_dir).isDir()) {
        resolver_->rememberPackageRoot(package.toStdString(), package_dir);
      } else {
        missing.push_back(package);
      }
    }
    refresh_roots_list();
    if (missing.size() == packages.size()) {
      setStatus(
          tr("Package '%1' not found under '%2' - expected a subdirectory named '%1'").arg(missing.first(), root));
      refresh_status();
      return;
    }
    loadFromCurrentSource();
    refresh_status();
  });
  connect(retry_button, &QToolButton::clicked, this, [this, refresh_status]() {
    loadFromCurrentSource();
    refresh_status();
  });
  connect(
      prefix_edit, &QLineEdit::editingFinished, this, [this, prefix_edit]() { setFramePrefix(prefix_edit->text()); });
  connect(mode_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, mode_combo](int) {
    setDisplayMode(static_cast<DisplayMode>(mode_combo->currentData().toInt()));
  });
  connect(color_button, &QPushButton::clicked, this, [this, container, color_button]() {
    auto* popup = new PJ::ColorPickerPopup(container);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setColor(fallback_color_);
    connect(popup, &PJ::ColorPickerPopup::colorChanged, this, [this, color_button](QColor color) {
      if (color.isValid()) {
        setFallbackColor(color);
        color_button->setColor(color);
      }
    });
    popup->move(color_button->mapToGlobal(QPoint(0, color_button->height() + 2)));
    popup->show();
  });
  connect(collada_box, &QCheckBox::toggled, this, &RobotModelLayer::setIgnoreColladaUpAxis);
  connect(this, &RobotModelLayer::statusTextChanged, container, [refresh_status](const QString&) { refresh_status(); });
  connect(this, &RobotModelLayer::meshLoadStatusChanged, container, [refresh_status](int, int, const QStringList&) {
    refresh_status();
  });

  return container;
}

void RobotModelLayer::setPackageResolver(UrdfPackageResolver* resolver) {
  resolver_ = resolver != nullptr ? resolver : owned_resolver_.get();
}

void RobotModelLayer::setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name) {
  source_type_ = SourceType::kTopic;
  source_topic_id_ = topic_id;
  if (!display_name.isEmpty()) {
    display_name_ = std::move(display_name);
    source_value_ = display_name_;
  }
  if (ctx_.session != nullptr) {
    parser_ = ctx_.session->parserForObjectTopic(source_topic_id_);
    parser_mutex_ = ctx_.session->parserMutexForObjectTopic(source_topic_id_);
    if (parser_ == nullptr) {
      setStatus(tr("No parser for %1").arg(source_value_));
      return;
    }
    loadFromCurrentSource();
  }
}

void RobotModelLayer::setSourceFile(QString path) {
  source_type_ = SourceType::kFile;
  source_value_ = std::move(path);
  loadFromCurrentSource();
}

void RobotModelLayer::setSourceUrl(QString url) {
  source_type_ = SourceType::kUrl;
  source_value_ = std::move(url);
  loadFromCurrentSource();
}

void RobotModelLayer::setFramePrefix(QString prefix) {
  if (frame_prefix_ == prefix) {
    return;
  }
  frame_prefix_ = std::move(prefix);
  emit sourceFrameChanged(sourceFrame());
  emit fallbackFramesChanged(fallbackFrames());
  emit repaintRequested();
}

void RobotModelLayer::setDisplayMode(DisplayMode mode) {
  if (display_mode_ == mode) {
    return;
  }
  display_mode_ = mode;
  emit repaintRequested();
}

void RobotModelLayer::setFallbackColor(QColor color) {
  if (!color.isValid() || fallback_color_ == color) {
    return;
  }
  fallback_color_ = std::move(color);
  emit repaintRequested();
}

void RobotModelLayer::setIgnoreColladaUpAxis(bool ignore) {
  ignore_collada_up_axis_ = ignore;
  emit repaintRequested();
}

QString RobotModelLayer::linkFrameName(const std::string& link_name) const {
  return frame_prefix_ + QString::fromStdString(link_name);
}

bool RobotModelLayer::loadFromCurrentSource() {
  model_.reset();
  mesh_loads_.clear();
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  loaded_mesh_count_ = 0;
  latch_pending_ = false;
  if (mesh_pass_) {
    mesh_pass_->clearMeshes();
  }

  if (source_type_ == SourceType::kTopic) {
    return tryLoadTopicDescription();
  }

  QString error;
  std::optional<QString> text;
  QString label = source_value_;
  QString urdf_dir;
  bool source_is_url = false;
  if (source_type_ == SourceType::kFile) {
    text = readTextFile(source_value_, &error);
    urdf_dir = QFileInfo(source_value_).absolutePath();
  } else {
    setStatus(tr("Fetching %1").arg(source_value_));
    text = readUrlBlocking(source_value_, &error);
    urdf_dir = urlDirectory(source_value_);
    source_is_url = true;
  }
  if (!text.has_value()) {
    setStatus(
        source_type_ == SourceType::kUrl ? tr("Fetch failed (%1)").arg(error)
                                         : tr("Failed to read URDF: %1").arg(error));
    return false;
  }
  return applyRobotDescription(*text, formatFromXml(*text), label, urdf_dir, source_is_url);
}

bool RobotModelLayer::tryLoadTopicDescription() {
  if (ctx_.session == nullptr || parser_ == nullptr) {
    return false;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  const auto entry = store.latestAt(source_topic_id_, std::numeric_limits<PJ::Timestamp>::max());
  if (!entry.has_value() || entry->payload.bytes.empty()) {
    latch_pending_ = true;
    last_latch_retry_ = std::chrono::steady_clock::now();
    const QString topic =
        source_value_.isEmpty() ? QString::fromStdString(store.descriptor(source_topic_id_).topic_name) : source_value_;
    setStatus(tr("Waiting for %1...").arg(topic));
    return true;
  }

  auto obj = parseLocked(parser_, parser_mutex_, entry->timestamp, entry->payload);
  if (!obj.has_value()) {
    setStatus(tr("Parse error: %1").arg(QString::fromStdString(obj.error())));
    return false;
  }
  const auto* desc = std::any_cast<PJ::sdk::RobotDescription>(&obj->object);
  if (desc == nullptr) {
    setStatus(tr("Parse error: parser did not return RobotDescription"));
    return false;
  }
  const QString label = source_value_.isEmpty() ? QString::fromStdString(desc->topic) : source_value_;
  return applyRobotDescription(
      QString::fromStdString(desc->text), QString::fromStdString(desc->format), label, QString(), false);
}

bool RobotModelLayer::applyRobotDescription(
    const QString& text, const QString& format, const QString& label, const QString& urdf_dir, bool source_is_url) {
  latch_pending_ = false;
  if (format.compare(QStringLiteral("urdf"), Qt::CaseInsensitive) != 0) {
    setStatus(tr("Format '%1' is not supported — only URDF").arg(format));
    return false;
  }

  if (resolver_ != nullptr) {
    resolver_->clearUnresolved();
    resolver_->autoSeedSearchRoots(urdf_dir);
  }
  auto parsed = parseUrdf(text.toStdString(), resolver_, urdf_dir.toStdString(), source_is_url, label.toStdString());
  if (!parsed.first.has_value()) {
    setStatus(QString::fromStdString(parsed.second));
    return false;
  }

  model_ = std::move(parsed.first);
  startMeshLoads();
  updateMeshCounters();
  const QStringList unresolved = unresolvedPackagesList();
  const QString mesh_status = unresolved_mesh_count_ > 0
                                  ? tr("URDF: %1  •  %2/%3 meshes  •  %4 packages unresolved")
                                        .arg(label)
                                        .arg(total_mesh_count_ - unresolved_mesh_count_)
                                        .arg(total_mesh_count_)
                                        .arg(unresolved.size())
                                  : tr("URDF: %1  •  %2/%2 meshes").arg(label).arg(total_mesh_count_);
  setStatus(mesh_status);
  emit sourceFrameChanged(sourceFrame());
  emit fallbackFramesChanged(fallbackFrames());
  emit unresolvedPackages(unresolved);
  emit meshLoadStatusChanged(loaded_mesh_count_, total_mesh_count_, unresolved);
  emit repaintRequested();
  return true;
}

void RobotModelLayer::startMeshLoads() {
  if (!model_.has_value() || mesh_loader_ == nullptr) {
    return;
  }
  std::vector<std::string> started;
  auto start = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr) {
      return;
    }
    ++total_mesh_count_;
    if (!mesh->resolved || mesh->resolved_path.empty()) {
      ++unresolved_mesh_count_;
      return;
    }
    if (std::find(started.begin(), started.end(), mesh->resolved_path) != started.end()) {
      return;
    }
    started.push_back(mesh->resolved_path);
    auto record = std::make_unique<MeshLoadRecord>();
    record->key = mesh->resolved_path;
    record->path = QString::fromStdString(mesh->resolved_path);
    record->future = mesh_loader_->load(record->path);
    // The watcher lives on this (GUI) thread; connect BEFORE setFuture so an
    // already-finished load (cache hit) still signals.
    record->watcher = std::make_unique<QFutureWatcher<MeshData>>();
    connect(record->watcher.get(), &QFutureWatcher<MeshData>::finished, this, &RobotModelLayer::pollMeshLoads);
    record->watcher->setFuture(record->future);
    mesh_loads_.push_back(std::move(record));
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geom : link.visuals) {
      start(geom);
    }
    for (const LinkGeom& geom : link.collisions) {
      start(geom);
    }
  }
}

void RobotModelLayer::pollMeshLoads() {
  if (!mesh_pass_) {
    return;
  }
  bool changed = false;
  for (const auto& record : mesh_loads_) {
    if (record->consumed || !record->future.isFinished()) {
      continue;
    }
    MeshData data = record->future.result();
    record->failed = !data.ok;
    if (data.ok) {
      mesh_pass_->setMeshData(record->key, std::move(data));
      ++loaded_mesh_count_;
    }
    record->consumed = true;
    changed = true;
  }
  if (changed) {
    emit meshLoadStatusChanged(loaded_mesh_count_, total_mesh_count_, unresolvedPackagesList());
    // Swap the placeholder for the loaded mesh now — meshLoadStatusChanged only
    // feeds the config-widget label and schedules no paint.
    emit repaintRequested();
  }
}

RobotModelLayer::MeshLoadRecord* RobotModelLayer::meshLoadForKey(const std::string& key) const {
  for (const auto& record : mesh_loads_) {
    if (record->key == key) {
      return record.get();
    }
  }
  return nullptr;
}

bool RobotModelLayer::meshReady(const std::string& key) const {
  const MeshLoadRecord* record = meshLoadForKey(key);
  return record != nullptr && record->consumed && !record->failed;
}

void RobotModelLayer::updateMeshCounters() {
  if (!model_.has_value()) {
    return;
  }
  total_mesh_count_ = 0;
  unresolved_mesh_count_ = 0;
  auto count = [&](const LinkGeom& geom) {
    const auto* mesh = std::get_if<GeomMesh>(&geom.shape);
    if (mesh == nullptr) {
      return;
    }
    ++total_mesh_count_;
    if (!mesh->resolved) {
      ++unresolved_mesh_count_;
    }
  };
  for (const RobotLink& link : model_->links) {
    for (const LinkGeom& geom : link.visuals) {
      count(geom);
    }
    for (const LinkGeom& geom : link.collisions) {
      count(geom);
    }
  }
}

void RobotModelLayer::setStatus(QString status) {
  if (status_text_ == status) {
    return;
  }
  status_text_ = std::move(status);
  emit statusTextChanged(status_text_);
}

QStringList RobotModelLayer::unresolvedPackagesList() const {
  return resolver_ != nullptr ? resolver_->unresolvedPackages() : QStringList{};
}

}  // namespace pj::scene3d
