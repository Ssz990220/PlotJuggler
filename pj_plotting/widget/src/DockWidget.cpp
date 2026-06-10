// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/DockWidget.h"

#include <DockAreaWidget.h>
#include <DockManager.h>

#include <QAction>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QUuid>
#include <QWidget>
#include <utility>

#include "pj_plotting/DockToolbar.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"

namespace PJ {
namespace {

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isContentWidgetOrChild(QObject* watched, QWidget* content_widget) {
  auto* widget = qobject_cast<QWidget*>(watched);
  return widget != nullptr && content_widget != nullptr &&
         (widget == content_widget || content_widget->isAncestorOf(widget));
}

}  // namespace

DockWidget::DockWidget(SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent)
    : DockWidget(nullptr, session, catalog, manager, parent, false) {}

DockWidget::DockWidget(
    PlotWidget* plot, SessionManager* session, CatalogModel* catalog, ads::CDockManager* manager, QWidget* parent,
    bool create_plot_when_null)
    : ads::CDockWidget(manager, "Plot", parent != nullptr ? parent : manager),
      session_(session),
      catalog_(catalog),
      state_id_(newStateId()) {
  setFrameShape(QFrame::NoFrame);

  setFeature(ads::CDockWidget::DockWidgetFloatable, false);
  setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);

  toolbar_ = new DockToolbar(this);
  toolbar_->label()->setText("...");
  qobject_cast<QBoxLayout*>(layout())->insertWidget(0, toolbar_);

  connect(toolbar_->buttonSplitHorizontal(), &QPushButton::clicked, this, [this]() { splitHorizontal(); });
  connect(toolbar_->buttonSplitVertical(), &QPushButton::clicked, this, [this]() { splitVertical(); });

  auto fullscreenAction = [this]() {
    auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
    if (!parent_docker) {
      return;
    }
    toolbar_->toggleFullscreen();
    const bool fullscreen = toolbar_->isFullscreen();
    for (int i = 0; i < parent_docker->dockAreaCount(); ++i) {
      auto* area = parent_docker->dockArea(i);
      if (area != dockAreaWidget()) {
        area->setVisible(!fullscreen);
      }
      toolbar_->buttonClose()->setHidden(fullscreen);
    }
  };
  connect(toolbar_->buttonFullscreen(), &QPushButton::clicked, this, fullscreenAction);

  connect(toolbar_->buttonClose(), &QPushButton::pressed, this, [this]() {
    dockAreaWidget()->closeArea();
    clearCurrentContent(true);
    emit undoableChange();
  });

  layout()->setContentsMargins(10, 10, 10, 10);
  if (plot != nullptr || create_plot_when_null) {
    setPlotWidget(plot != nullptr ? plot : new PlotWidget(session_, catalog_, this));
  } else {
    setPlaceholderWidget();
  }
}

DockWidget::~DockWidget() = default;

void DockWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  if (plot_widget_ != nullptr) {
    plot_widget_->setDataServices(session_, catalog_);
  }
}

void DockWidget::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
}

PlotWidget* DockWidget::plotWidget() {
  return plot_widget_;
}

IDataWidget* DockWidget::objectWidget() {
  return object_widget_;
}

PlotWidget* DockWidget::releasePlotWidget() {
  if (plot_widget_ == nullptr) {
    return nullptr;
  }
  disconnect(plot_widget_, nullptr, this, nullptr);
  auto* plot = plot_widget_;
  takeWidget();
  content_widget_ = nullptr;
  plot_widget_ = nullptr;
  return plot;
}

void DockWidget::setPlotWidget(PlotWidget* plot) {
  if (plot_widget_ == plot) {
    return;
  }
  clearCurrentContent(true);
  plot_widget_ = plot;
  content_widget_ = plot_widget_;
  if (plot_widget_ == nullptr) {
    return;
  }
  plot_widget_->setDataServices(session_, catalog_);
  setWidget(plot_widget_);
  connect(plot_widget_, &PlotWidget::splitHorizontal, this, [this]() { splitHorizontal(); });
  connect(plot_widget_, &PlotWidget::splitVertical, this, [this]() { splitVertical(); });
  connect(plot_widget_, &PlotWidget::undoableChange, this, &DockWidget::undoableChange);
  emit plotWidgetCreated(plot_widget_);
}

void DockWidget::setObjectWidget(IDataWidget* widget) {
  if (object_widget_ == widget) {
    return;
  }
  clearCurrentContent(true);
  object_widget_ = widget;
  content_widget_ = object_widget_ != nullptr ? object_widget_->widget() : nullptr;
  if (content_widget_ == nullptr) {
    return;
  }
  installObjectContextMenuFilter(content_widget_);
  // ForceNoScrollArea: object widgets manage their own viewport — wrapping
  // them in ADS' QScrollArea adds a visible frame. Mirrors the drop path.
  setWidget(content_widget_, ads::CDockWidget::ForceNoScrollArea);
}

IDataWidget* DockWidget::releaseObjectWidget() {
  if (object_widget_ == nullptr) {
    return nullptr;
  }
  if (content_widget_ != nullptr) {
    removeObjectContextMenuFilter(content_widget_);
  }
  auto* obj = object_widget_;
  takeWidget();
  content_widget_ = nullptr;
  object_widget_ = nullptr;
  return obj;
}

void DockWidget::setPlaceholderWidget() {
  clearCurrentContent(true);
  placeholder_widget_ = new VisualizationPlaceholderWidget(this);
  content_widget_ = placeholder_widget_;
  setWidget(placeholder_widget_);
  setName(QStringLiteral("..."));
  connect(
      placeholder_widget_, &VisualizationPlaceholderWidget::catalogItemsDropped, this,
      &DockWidget::onCatalogItemsDropped);
  connect(placeholder_widget_, &VisualizationPlaceholderWidget::splitHorizontalRequested, this, [this]() {
    splitHorizontal();
  });
  connect(placeholder_widget_, &VisualizationPlaceholderWidget::splitVerticalRequested, this, [this]() {
    splitVertical();
  });
}

DockToolbar* DockWidget::toolBar() {
  return toolbar_;
}

QString DockWidget::name() const {
  return toolbar_->label()->text();
}

void DockWidget::setName(const QString& name) {
  toolbar_->label()->setText(name);
}

QString DockWidget::stateId() const {
  return state_id_;
}

void DockWidget::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

void DockWidget::onTrackerTime(double time) {
  if (plot_widget_ != nullptr) {
    plot_widget_->setTrackerPosition(time);
  }
  if (object_widget_ != nullptr) {
    object_widget_->onTrackerTime(time);
  }
}

void DockWidget::onStylesheetChanged(QString theme) {
  if (toolbar_ != nullptr) {
    toolbar_->onStylesheetChanged(theme);
  }
  if (placeholder_widget_ != nullptr) {
    placeholder_widget_->onStylesheetChanged(theme);
  }
}

bool DockWidget::eventFilter(QObject* watched, QEvent* event) {
  if (object_widget_ != nullptr && content_widget_ != nullptr && isContentWidgetOrChild(watched, content_widget_) &&
      event != nullptr) {
    switch (event->type()) {
      case QEvent::ContextMenu: {
        auto* context_event = static_cast<QContextMenuEvent*>(event);
        showObjectContextMenu(context_event->globalPos());
        event->accept();
        return true;
      }
      // Catalog drops on the *live* content widget — once the placeholder
      // is gone, this filter is the only thing that hears the drop. Used by
      // multi-topic widgets (Scene3D) to absorb additional topics. The
      // factory replacement path stays as a fallback inside
      // onCatalogItemsDropped when the existing widget refuses the family.
      case QEvent::DragEnter:
      case QEvent::DragMove: {
        auto* drag = static_cast<QDropEvent*>(event);
        if (drag->mimeData() != nullptr && drag->mimeData()->hasFormat(CurveTreeView::catalogItemsMimeType())) {
          drag->acceptProposedAction();
          return true;
        }
        return false;
      }
      case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        const QStringList keys = CurveTreeView::decodeCatalogKeys(drop->mimeData());
        if (keys.isEmpty()) {
          return false;
        }
        drop->acceptProposedAction();
        onCatalogItemsDropped(keys);
        return true;
      }
      default:
        break;
    }
  }
  return ads::CDockWidget::eventFilter(watched, event);
}

DockWidget* DockWidget::splitHorizontal() {
  return splitHorizontal(nullptr);
}

DockWidget* DockWidget::splitVertical() {
  return splitVertical(nullptr);
}

DockWidget* DockWidget::splitHorizontal(PlotWidget* plot) {
  return splitInto(ads::RightDockWidgetArea, plot);
}

DockWidget* DockWidget::splitVertical(PlotWidget* plot) {
  return splitInto(ads::BottomDockWidgetArea, plot);
}

DockWidget* DockWidget::splitInto(ads::DockWidgetArea dock_area, PlotWidget* plot) {
  auto* parent_docker = qobject_cast<PlotDocker*>(dockManager());
  if (!parent_docker) {
    return nullptr;
  }
  auto* new_widget = new DockWidget(plot, session_, catalog_, parent_docker, nullptr, false);
  new_widget->setObjectWidgetFactory(object_widget_factory_);
  auto* area = parent_docker->addDockWidget(dock_area, new_widget, dockAreaWidget());
  area->setAllowedAreas(ads::OuterDockAreas);

  connect(new_widget, &DockWidget::undoableChange, parent_docker, &PlotDocker::undoableChange);
  connect(new_widget, &DockWidget::plotWidgetCreated, parent_docker, &PlotDocker::plotWidgetAdded);
  emit undoableChange();
  emit parent_docker->dockAdded(new_widget);
  if (new_widget->plotWidget() != nullptr) {
    emit parent_docker->plotWidgetAdded(new_widget->plotWidget());
  }
  return new_widget;
}

PlotWidget* DockWidget::ensurePlotWidget() {
  if (plot_widget_ == nullptr) {
    setPlotWidget(new PlotWidget(session_, catalog_, this));
    setName(QStringLiteral("..."));
  }
  return plot_widget_;
}

void DockWidget::onCatalogItemsDropped(const QStringList& keys) {
  if (catalog_ == nullptr || keys.empty()) {
    return;
  }

  const auto first_item = catalog_->itemDescriptor(keys.front());
  if (!first_item.has_value()) {
    return;
  }

  if (isScalarField(*first_item)) {
    PlotWidget* plot = ensurePlotWidget();
    bool changed = false;
    for (const QString& key : keys) {
      if (catalog_->curveDescriptor(key).has_value()) {
        changed = plot->addCurve(key) != nullptr || changed;
      }
    }
    if (changed) {
      plot->zoomOut(true);
      emit undoableChange();
    }
    focusSelf();
    return;
  }

  const auto* object_payload = asObjectTopic(*first_item);
  if (object_payload == nullptr || !object_widget_factory_) {
    return;
  }

  // Helper: build the title string the factory expects from a catalog item.
  // With a single dataset loaded the dataset name is redundant noise in the
  // object lists, so show just the topic; with multiple datasets keep the
  // "dataset/topic" qualifier to disambiguate.
  const bool single_dataset = (catalog_ != nullptr) && catalog_->datasets().size() <= 1;
  const auto title_for = [single_dataset](const auto& descriptor) {
    return (single_dataset || descriptor.dataset_name.isEmpty())
               ? descriptor.topic_name
               : QStringLiteral("%1/%2").arg(descriptor.dataset_name, descriptor.topic_name);
  };
  // Helper: walk all keys and offer each one to the given widget via
  // IDataWidget::tryAcceptObjectTopic. Returns the count accepted.
  const auto offer_keys_to = [&](IDataWidget* target, int start_index) {
    int accepted = 0;
    for (int i = start_index; i < keys.size(); ++i) {
      const auto desc = catalog_->itemDescriptor(keys[i]);
      if (!desc.has_value()) {
        continue;
      }
      const auto* payload = asObjectTopic(*desc);
      if (payload == nullptr) {
        continue;
      }
      if (target->tryAcceptObjectTopic(payload->object_topic_id, payload->object_type, title_for(*desc))) {
        ++accepted;
      }
    }
    return accepted;
  };

  // First: if a widget is already mounted, try to add the dropped topics
  // *into* it (Scene3DDockWidget consumes pointcloud / TF this way). On
  // success we don't replace the widget — only the topic list grows.
  if (object_widget_ != nullptr) {
    if (offer_keys_to(object_widget_, /*start_index=*/0) > 0) {
      emit undoableChange();
      focusSelf();
      return;
    }
    // Existing widget refused all keys (wrong family) — fall through and
    // replace it with a fresh factory-created one.
  }

  // The factory itself decides which object types it can host — returning
  // nullptr means "I can't render this", which we surface by reverting to
  // the placeholder so the user sees an explicit "not supported" affordance.
  const QString title = title_for(*first_item);
  clearCurrentContent(true);
  // Drop path: empty kind + a seed for the first topic. The factory classifies
  // the object type, constructs the matching dock, and populates the seed.
  const ObjectDropSeed seed{object_payload->object_topic_id, object_payload->object_type, title};
  object_widget_ = object_widget_factory_(QString(), &seed, this);
  content_widget_ = object_widget_ != nullptr ? object_widget_->widget() : nullptr;
  if (content_widget_ == nullptr) {
    setPlaceholderWidget();
    return;
  }
  installObjectContextMenuFilter(content_widget_);
  // ForceNoScrollArea: object widgets (Scene3DDockWidget, Scene2DDockWidget,
  // …) manage their own viewport — wrapping them in ADS' QScrollArea adds a
  // visible frame around the content. The object widget is responsible for
  // its own sizing/scrolling if any is needed.
  setWidget(content_widget_, ads::CDockWidget::ForceNoScrollArea);
  setName(first_item->topic_name);
  // Multi-select drop: hand the remaining keys to the new widget so it
  // can absorb the rest of the selection (Scene3D / future multi-topic
  // viewers benefit; single-topic widgets refuse and we drop them
  // silently — the user still got their first topic shown).
  if (object_widget_ != nullptr && keys.size() > 1) {
    offer_keys_to(object_widget_, /*start_index=*/1);
  }
  emit undoableChange();
  focusSelf();
}

void DockWidget::clearToPlaceholder() {
  setPlaceholderWidget();
  emit undoableChange();
}

void DockWidget::focusSelf() {
  if (auto* docker = qobject_cast<PlotDocker*>(dockManager()); docker != nullptr) {
    docker->focusDock(this);
  }
}

void DockWidget::clearCurrentContent(bool delete_content) {
  if (plot_widget_ != nullptr) {
    disconnect(plot_widget_, nullptr, this, nullptr);
  }
  if (placeholder_widget_ != nullptr) {
    disconnect(placeholder_widget_, nullptr, this, nullptr);
  }
  if (content_widget_ != nullptr) {
    removeObjectContextMenuFilter(content_widget_);
    takeWidget();
    if (delete_content) {
      content_widget_->deleteLater();
    }
  }
  content_widget_ = nullptr;
  placeholder_widget_ = nullptr;
  plot_widget_ = nullptr;
  object_widget_ = nullptr;
}

void DockWidget::installObjectContextMenuFilter(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  root->installEventFilter(this);
  // Enable drop-target status on the root content widget so subsequent
  // catalog drags surface DragEnter/Drop events here (eventFilter then
  // routes them to onCatalogItemsDropped). Children typically refuse drops
  // and Qt walks up the parent chain to find this accepting root, so we
  // don't need to flip every descendant — only the root.
  root->setAcceptDrops(true);
  const auto children = root->findChildren<QWidget*>();
  for (auto* child : children) {
    child->installEventFilter(this);
  }
}

void DockWidget::removeObjectContextMenuFilter(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  root->removeEventFilter(this);
  const auto children = root->findChildren<QWidget*>();
  for (auto* child : children) {
    child->removeEventFilter(this);
  }
}

void DockWidget::showObjectContextMenu(const QPoint& global_pos) {
  const QString theme = currentTheme();
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("PJMenu"));
  menu.addAction(QIcon(LoadSvg(":/resources/svg/add_column.svg", theme)), tr("Split Horizontally"), this, [this]() {
    splitHorizontal();
  });
  menu.addAction(QIcon(LoadSvg(":/resources/svg/add_row.svg", theme)), tr("Split Vertically"), this, [this]() {
    splitVertical();
  });
  menu.addSeparator();
  menu.addAction(
      QIcon(LoadSvg(":/resources/svg/clear.svg", theme)), tr("Clear"), this, [this]() { clearToPlaceholder(); });
  menu.exec(global_pos);
}

}  // namespace PJ
