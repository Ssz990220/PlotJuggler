#include "pj_scene2d_widgets/Media2DDockWidget.h"

#include <QBoxLayout>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QString>

#include "pj_base/builtin/asset_video.hpp"
#include "pj_base/builtin/asset_video_codec.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/file_video_source.h"
#include "pj_scene2d_core/image_pipeline_source.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcMedia2DDock, "pj.scene2d.dock")
}  // namespace

Media2DDockWidget::Media2DDockWidget(QWidget* parent) : QWidget(parent) {
  setWindowTitle(tr("2D View"));

  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // QRhi bootstrap: keep this before the visible widget so the first dynamic
  // media view does not reinitialize the whole top-level window surface.
  bootstrap_ = new MediaViewerWidget(this);
  bootstrap_->setMaximumSize(0, 0);
  main_layout->addWidget(bootstrap_);

  viewer_ = new MediaViewerWidget(this);
  viewer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  main_layout->addWidget(viewer_);
}

Media2DDockWidget::~Media2DDockWidget() {
  // Detach the viewer's media_source_ pointer BEFORE joining the worker (or
  // FfmpegBackend decode thread for FileVideoSource) so any in-flight render()
  // finishes without touching the source about to be destroyed. Resetting the
  // unique_ptr next blocks until the worker joins.
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(nullptr);
  }
  media_topic_source_.reset();
}

void Media2DDockWidget::setSessionManager(SessionManager* session) {
  session_ = session;
}

void Media2DDockWidget::setPointInspectorEnabled(bool enabled) {
  if (viewer_ != nullptr) {
    viewer_->setPointInspectorEnabled(enabled);
  }
}

bool Media2DDockWidget::pointInspectorEnabled() const noexcept {
  return viewer_ != nullptr && viewer_->pointInspectorEnabled();
}

bool Media2DDockWidget::setImageTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (session_ == nullptr) {
    qCWarning(lcMedia2DDock) << "setImageTopic: session is null — drop ignored (topic_id=" << topic_id.id
                             << "object_type=" << static_cast<int>(object_type) << ")";
    return false;
  }

  // Detach any previous source from the viewer BEFORE destroying it, mirroring
  // the destructor discipline so a render mid-rebind cannot touch a freed
  // source.
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(nullptr);
  }
  media_topic_source_.reset();

  ObjectStore& store = session_->objectStore();
  topic_id_ = topic_id;

  // Video branch: file-backed video declared as a single sdk::AssetVideo entry
  // in ObjectStore. The MP4 itself is the random-access store; decoding the
  // asset payload yields the file path and (optional) wall-clock anchor.
  if (object_type == sdk::BuiltinObjectType::kAssetVideo) {
    if (store.entryCount(topic_id) == 0) {
      qCWarning(lcMedia2DDock) << "setImageTopic: kAssetVideo topic_id=" << topic_id.id
                               << "has no entries — drop refused (producer must push the AssetVideo entry at "
                                  "registration time)";
      return false;
    }
    const auto entry = store.at(topic_id, 0);
    if (!entry.has_value() || entry->data == nullptr) {
      qCWarning(lcMedia2DDock) << "setImageTopic: kAssetVideo topic_id=" << topic_id.id << "first entry has no payload";
      return false;
    }
    auto asset = PJ::deserializeAssetVideo(entry->data->data(), entry->data->size());
    if (!asset.has_value()) {
      qCWarning(lcMedia2DDock) << "setImageTopic: deserializeAssetVideo failed for topic_id=" << topic_id.id << ":"
                               << QString::fromStdString(asset.error());
      return false;
    }
    auto src = FileVideoSource::open(asset->file_path);
    if (!src.has_value()) {
      qCWarning(lcMedia2DDock) << "setImageTopic: FileVideoSource::open failed for"
                               << QString::fromStdString(asset->file_path) << ":"
                               << QString::fromStdString(src.error());
      return false;
    }
    if (asset->time_origin_ns.has_value()) {
      (*src)->setEpochAnchorNs(*asset->time_origin_ns);
    }
    media_topic_source_ = std::move(*src);
    viewer_->setMediaSource(media_topic_source_.get());
    setWindowTitle(title.isEmpty() ? tr("2D View") : tr("2D View - %1").arg(title));
    // Bootstrap at file PTS 0. With the anchor applied, setTimestamp(anchor)
    // maps to file-relative 0. Unanchored video uses anchor=0 → setTimestamp(0).
    media_topic_source_->setTimestamp(asset->time_origin_ns.value_or(0));
    viewer_->update();
    return true;
  }

  // Image branch (parser-driven canonical kImage, or built-in JPEG pipeline).
  auto pipeline = makePipelineFor(object_type);
  auto* parser = session_->parserForObjectTopic(topic_id);
  if (parser == nullptr && pipeline == nullptr) {
    // Production object topics are expected to ship a parser via the data
    // source's parser registrar. The pipeline fallback only exists for image
    // types that have a built-in decoder family (currently kImage→JPEG).
    // kDepthImage and other canonical types must come from a parser; rather
    // than re-introducing source-format repair logic here, we surface the
    // missing parser loudly so the operator can see why the dock stayed
    // empty. TODO(scene2d): wire a deliberate sdk::DepthImage display
    // pipeline that consumes canonical parser output.
    qCWarning(lcMedia2DDock) << "setImageTopic: no parser registered for topic_id=" << topic_id.id
                             << "object_type=" << static_cast<int>(object_type)
                             << "and no built-in pipeline supports this type — drop refused";
    return false;
  }

  std::unique_ptr<ImagePipelineSource> image_src;
  if (parser != nullptr) {
    image_src = std::make_unique<ImagePipelineSource>(&store, topic_id, parser);
  } else {
    image_src = std::make_unique<ImagePipelineSource>(&store, topic_id, std::move(pipeline));
  }

  // Callback fires from the source's worker thread; hop to the GUI thread via
  // QueuedConnection and consume the frame in pollPendingFrame(). QPointer
  // guards against the widget being deleted between worker emission and slot
  // invocation; invokeMethod against a destroyed receiver is also safe by
  // itself but the QPointer check avoids posting an event we know is moot.
  // setFrameReadyCallback only exists on ImagePipelineSource — install while
  // we still hold the concrete type, then erase into the polymorphic member.
  image_src->setFrameReadyCallback([qp = QPointer<Media2DDockWidget>(this)]() {
    if (qp) {
      QMetaObject::invokeMethod(qp.data(), "pollPendingFrame", Qt::QueuedConnection);
    }
  });

  media_topic_source_ = std::move(image_src);
  viewer_->setMediaSource(media_topic_source_.get());
  setWindowTitle(title.isEmpty() ? tr("2D View") : tr("2D View - %1").arg(title));

  if (store.entryCount(topic_id) == 0) {
    qCInfo(lcMedia2DDock) << "setImageTopic: topic_id=" << topic_id.id
                          << "has no entries yet — viewer attached, will render once data arrives";
    return true;
  }

  // Kick off the bootstrap frame asynchronously — the callback above will land
  // it in the viewer once the worker is done.
  const auto range = store.timeRange(topic_id);
  media_topic_source_->setTimestamp(range.first);
  return true;
}

void Media2DDockWidget::onTrackerTime(double time) {
  constexpr double kNsPerSec = 1.0e9;
  const auto ts = static_cast<int64_t>(time * kNsPerSec);
  if (viewer_ == nullptr) {
    return;
  }

  if (media_topic_source_ != nullptr && session_ != nullptr) {
    // ImagePipelineSource: cheap (microseconds) — posts a target to its
    // worker thread; the decoded frame lands in pollPendingFrame() when the
    // worker fires its frame-ready callback.
    // FileVideoSource: also cheap — applies the epoch anchor and posts a
    // seek to FfmpegBackend's decode thread. The next render() polls
    // takeFrame() via processEvents() and surfaces the frame; the explicit
    // update() below schedules that repaint. update() is coalesced by Qt
    // so the redundant call on the image branch is a no-op.
    media_topic_source_->setTimestamp(ts);
    viewer_->update();
    return;
  }

  viewer_->setTimestamp(ts);
  viewer_->update();
}

void Media2DDockWidget::pollPendingFrame() {
  // Only ImagePipelineSource installs a frame-ready callback that routes here.
  // FileVideoSource lets MediaViewerWidget::render() poll takeFrame() directly
  // (via processEvents()), so this slot is effectively a no-op for the video
  // branch.
  if (viewer_ == nullptr || media_topic_source_ == nullptr) {
    return;
  }
  auto frame = media_topic_source_->takeFrame();
  if (!frame.has_value() || !frame->base.has_value()) {
    // Worker may fire the callback once per decode, but the latest result
    // was already taken by an earlier invocation (e.g. rapid coalesced
    // decodes). Treat as a no-op.
    return;
  }
  viewer_->setFrame(*frame->base);
  viewer_->update();
}

std::unique_ptr<CodecPipeline> Media2DDockWidget::makePipelineFor(sdk::BuiltinObjectType object_type) {
  switch (object_type) {
    case sdk::BuiltinObjectType::kImage:
      return makeJpegPipeline();
    case sdk::BuiltinObjectType::kDepthImage:
      return nullptr;
    case sdk::BuiltinObjectType::kNone:
    case sdk::BuiltinObjectType::kPointCloud:
    case sdk::BuiltinObjectType::kImageAnnotations:
    case sdk::BuiltinObjectType::kFrameTransforms:
    case sdk::BuiltinObjectType::kOccupancyGrid:
    case sdk::BuiltinObjectType::kCompressedPointCloud:
    case sdk::BuiltinObjectType::kMesh3D:
    case sdk::BuiltinObjectType::kVideoFrame:
    case sdk::BuiltinObjectType::kSceneEntities:
    case sdk::BuiltinObjectType::kAssetVideo:
    case sdk::BuiltinObjectType::kRobotDescription:
      return nullptr;
  }
  return nullptr;
}

}  // namespace PJ
