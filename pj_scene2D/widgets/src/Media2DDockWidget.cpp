#include "pj_scene2d_widgets/Media2DDockWidget.h"

#include <QBoxLayout>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/codecs.h"
#include "pj_scene2d_core/image_pipeline_source.h"
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
  // Detach the viewer's media_source_ pointer BEFORE joining the worker so
  // any in-flight render() finishes without touching the source that's about
  // to be destroyed. Resetting the unique_ptr next blocks until the worker
  // thread joins, so no further callback / takeFrame can race destruction.
  if (viewer_ != nullptr) {
    viewer_->setMediaSource(nullptr);
  }
  image_topic_source_.reset();
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

  ObjectStore& store = session_->objectStore();
  if (parser != nullptr) {
    image_topic_source_ = std::make_unique<ImagePipelineSource>(&store, topic_id, parser);
  } else {
    image_topic_source_ = std::make_unique<ImagePipelineSource>(&store, topic_id, std::move(pipeline));
  }
  topic_id_ = topic_id;

  // Callback fires from the source's worker thread; hop to the GUI thread via
  // QueuedConnection and consume the frame in pollPendingFrame(). QPointer
  // guards against the widget being deleted between worker emission and slot
  // invocation; invokeMethod against a destroyed receiver is also safe by
  // itself but the QPointer check avoids posting an event we know is moot.
  image_topic_source_->setFrameReadyCallback([qp = QPointer<Media2DDockWidget>(this)]() {
    if (qp) {
      QMetaObject::invokeMethod(qp.data(), "pollPendingFrame", Qt::QueuedConnection);
    }
  });

  viewer_->setMediaSource(image_topic_source_.get());
  setWindowTitle(title.isEmpty() ? tr("2D View") : tr("2D View - %1").arg(title));

  if (store.entryCount(topic_id) == 0) {
    qCInfo(lcMedia2DDock) << "setImageTopic: topic_id=" << topic_id.id
                          << "has no entries yet — viewer attached, will render once data arrives";
    return true;
  }

  // Kick off the bootstrap frame asynchronously — the callback above will land
  // it in the viewer once the worker is done. setTimestamp returns in microseconds.
  const auto range = store.timeRange(topic_id);
  image_topic_source_->setTimestamp(range.first);
  return true;
}

void Media2DDockWidget::onTrackerTime(double time) {
  constexpr double kNsPerSec = 1.0e9;
  const auto ts = static_cast<int64_t>(time * kNsPerSec);
  if (viewer_ == nullptr) {
    return;
  }

  if (image_topic_source_ != nullptr && session_ != nullptr) {
    // Cheap (microseconds): just posts a target to the worker thread. The
    // decoded frame lands in pollPendingFrame() asynchronously when the
    // worker fires its frame-ready callback.
    image_topic_source_->setTimestamp(ts);
    return;
  }

  viewer_->setTimestamp(ts);
  viewer_->update();
}

void Media2DDockWidget::pollPendingFrame() {
  if (viewer_ == nullptr || image_topic_source_ == nullptr) {
    return;
  }
  auto frame = image_topic_source_->takeFrame();
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
      return nullptr;
  }
  return nullptr;
}

}  // namespace PJ
