#include "pj_widgets/VisualizationPlaceholderWidget.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QMimeData>
#include <QSize>
#include <QToolButton>

#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {
namespace {

QToolButton* makeIconButton(const QString& icon_path, const QString& tooltip, bool enabled, QWidget* parent) {
  auto* button = new QToolButton(parent);
  // Icon is theme-tinted by the caller via LoadSvg+setIcon after the
  // button is created; the placeholder also re-tints on theme changes.
  Q_UNUSED(icon_path);
  button->setIconSize(QSize(48, 48));
  button->setFixedSize(66, 66);
  button->setAutoRaise(true);
  button->setFocusPolicy(Qt::NoFocus);
  button->setToolTip(tooltip);
  button->setEnabled(enabled);
  button->setCursor(Qt::ArrowCursor);
  return button;
}

bool acceptsCatalogItems(const QMimeData* mime_data) {
  return mime_data != nullptr && mime_data->hasFormat(CurveTreeView::catalogItemsMimeType());
}

bool acceptCatalogDrag(QDropEvent* event) {
  if (event != nullptr && acceptsCatalogItems(event->mimeData())) {
    event->acceptProposedAction();
    return true;
  }
  return false;
}

bool dropCatalogItems(QDropEvent* event, VisualizationPlaceholderWidget* target) {
  const QStringList keys = CurveTreeView::decodeCatalogKeys(event != nullptr ? event->mimeData() : nullptr);
  if (keys.empty()) {
    return false;
  }
  emit target->catalogItemsDropped(keys);
  event->acceptProposedAction();
  return true;
}

}  // namespace

VisualizationPlaceholderWidget::VisualizationPlaceholderWidget(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setObjectName(QStringLiteral("VisualizationPlaceholderWidget"));

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);
  layout->addStretch(1);
  const struct {
    const char* path;
    const char* tooltip;
    bool enabled;
  } specs[] = {
      {":/resources/svg/line_axis.svg", QT_TR_NOOP("Plot"), true},
      {":/resources/svg/image.svg", QT_TR_NOOP("2D"), true},
      {":/resources/svg/cube.svg", QT_TR_NOOP("3D"), false},
  };
  icon_buttons_.reserve(std::size(specs));
  for (const auto& spec : specs) {
    auto* button = makeIconButton(QString::fromLatin1(spec.path), tr(spec.tooltip), spec.enabled, this);
    button->setAcceptDrops(true);
    button->installEventFilter(this);
    layout->addWidget(button);
    icon_buttons_.push_back({button, QString::fromLatin1(spec.path)});
  }
  layout->addStretch(1);
  // Initial paint at whatever theme is currently active. Subsequent
  // changes flow in through onStylesheetChanged.
  onStylesheetChanged(currentTheme());
}

void VisualizationPlaceholderWidget::onStylesheetChanged(const QString& theme) {
  // RenderSvgPixmap (not LoadSvg) so the central icons rasterize at
  // exactly their display size (with DPR baked in) and stay crisp. The
  // shared LoadSvg cache always renders to 64x64, which is downsampled
  // to 48x48 here -- visible blur on the larger placeholder buttons.
  for (const auto& entry : icon_buttons_) {
    const QSize icon_size = entry.button->iconSize();
    const QPixmap pixmap = RenderSvgPixmap(entry.icon_path, theme, icon_size, devicePixelRatioF());
    entry.button->setIcon(QIcon(pixmap));
  }
}

bool VisualizationPlaceholderWidget::eventFilter(QObject* watched, QEvent* event) {
  Q_UNUSED(watched)
  if (event == nullptr) {
    return QWidget::eventFilter(watched, event);
  }

  switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
      return acceptCatalogDrag(static_cast<QDropEvent*>(event));
    case QEvent::Drop:
      return dropCatalogItems(static_cast<QDropEvent*>(event), this);
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
}

void VisualizationPlaceholderWidget::dragEnterEvent(QDragEnterEvent* event) {
  if (acceptCatalogDrag(event)) {
    return;
  }
  QWidget::dragEnterEvent(event);
}

void VisualizationPlaceholderWidget::dragMoveEvent(QDragMoveEvent* event) {
  if (acceptCatalogDrag(event)) {
    return;
  }
  QWidget::dragMoveEvent(event);
}

void VisualizationPlaceholderWidget::dropEvent(QDropEvent* event) {
  if (dropCatalogItems(event, this)) {
    return;
  }
  QWidget::dropEvent(event);
}

}  // namespace PJ
