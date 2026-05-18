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

namespace PJ {
namespace {

QToolButton* makeIconButton(const QString& icon_path, const QString& tooltip, bool enabled, QWidget* parent) {
  auto* button = new QToolButton(parent);
  button->setIcon(QIcon(icon_path));
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
  for (auto* button :
       {makeIconButton(QStringLiteral(":/resources/svg/scatter_plot.svg"), tr("Plot"), true, this),
        makeIconButton(QStringLiteral(":/resources/svg/cast.svg"), tr("2D"), true, this),
        makeIconButton(QStringLiteral(":/resources/svg/grid.svg"), tr("3D"), false, this)}) {
    button->setAcceptDrops(true);
    button->installEventFilter(this);
    layout->addWidget(button);
  }
  layout->addStretch(1);
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
