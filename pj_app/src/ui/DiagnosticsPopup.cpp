// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/DiagnosticsPopup.h"

#include <QClipboard>
#include <QEasingCurve>
#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "pj_widgets/SvgUtil.h"
#include "ui/DiagnosticsCard.h"
#include "ui_DiagnosticsPopup.h"

namespace PJ {

DiagnosticsPopup::DiagnosticsPopup(QWidget* parent) : QFrame(parent), ui_(new Ui::DiagnosticsPopup) {
  setAttribute(Qt::WA_StyledBackground, true);
  // Qt::Popup gets us outside-click-dismiss and implicit focus capture
  // for free. Frameless and no system shadows.
  setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
  ui_->setupUi(this);

  // Vertical scroll appears only when the inner contents exceed the
  // visible area. Beyond the visible cap the scrollbar takes over.
  ui_->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  ui_->scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Watch viewport resizes — we use them to clamp scrollContents to
  // the viewport width (see syncContentsWidthToViewport).
  ui_->scrollArea->viewport()->installEventFilter(this);

  // Pin cards to the top of the scroll contents. Replaces the trailing
  // stretch we used to insert in rebuildCards() — that pattern forced
  // a full re-add of every widget on each onRecorded() because the
  // stretch had to be removed and re-appended after the inserted card.
  // With AlignTop, new cards can be appended in O(1) without disturbing
  // existing widgets.
  ui_->cardsLayout->setAlignment(Qt::AlignTop);

  // The drag handle catches mouse events for user-driven resize.
  ui_->dragHandle->setCursor(Qt::SizeVerCursor);
  ui_->dragHandle->installEventFilter(this);

  open_animation_ = new QPropertyAnimation(this, "geometry", this);
  open_animation_->setDuration(160);
  open_animation_->setEasingCurve(QEasingCurve::OutCubic);

  appendEmptyStateCard();
}

DiagnosticsPopup::~DiagnosticsPopup() {
  delete ui_;
}

void DiagnosticsPopup::setHistory(DiagnosticHistory* history) {
  if (history_ != nullptr) {
    disconnect(history_, nullptr, this, nullptr);
  }
  history_ = history;
  if (history_ == nullptr) {
    return;
  }
  connect(history_, &DiagnosticHistory::recorded, this, &DiagnosticsPopup::onRecorded);
  connect(history_, &DiagnosticHistory::cleared, this, &DiagnosticsPopup::onHistoryCleared);
}

void DiagnosticsPopup::onRecorded(const DiagnosticRecord& r) {
  if (!isVisible()) {
    return;
  }
  // Empty-state → first card transition: drop the placeholder QLabel
  // and start fresh. Subsequent inserts append in O(1).
  if (cards_.isEmpty()) {
    clearCards();
    appendCard(r);
  } else {
    appendCard(r);
    // Honour the rolling-buffer cap maintained by DiagnosticHistory:
    // if our card list is now longer than the history's snapshot,
    // drop the oldest card from the front.
    if (history_ != nullptr) {
      while (!cards_.isEmpty() && cards_.size() > history_->size()) {
        DiagnosticsCard* oldest = cards_.takeFirst();
        ui_->cardsLayout->removeWidget(oldest);
        oldest->deleteLater();
      }
    }
  }
  applyEffectiveHeight();
}

void DiagnosticsPopup::onHistoryCleared() {
  if (isVisible()) {
    rebuildCards();
    applyEffectiveHeight();
  }
}

int DiagnosticsPopup::cardCount() const {
  return static_cast<int>(cards_.size());
}

bool DiagnosticsPopup::isShowingEmptyState() const {
  return history_ == nullptr || history_->isEmpty();
}

void DiagnosticsPopup::rebuildCards() {
  clearCards();
  if (history_ == nullptr || history_->isEmpty()) {
    appendEmptyStateCard();
    return;
  }
  for (const DiagnosticRecord& r : history_->snapshot()) {
    appendCard(r);
  }
  // No trailing stretch — cardsLayout uses Qt::AlignTop (set in the
  // ctor) so unused space falls below the cards without being
  // distributed between them.
}

void DiagnosticsPopup::clearCards() {
  while (ui_->cardsLayout->count() > 0) {
    QLayoutItem* item = ui_->cardsLayout->takeAt(0);
    if (item->widget() != nullptr) {
      item->widget()->deleteLater();
    }
    delete item;
  }
  cards_.clear();
}

void DiagnosticsPopup::appendCard(const DiagnosticRecord& item) {
  auto* card = new DiagnosticsCard(item, ui_->scrollContents);
  card->setObjectName(QStringLiteral("DiagnosticsCard"));
  connect(card, &DiagnosticsCard::activated, this, [this](const DiagnosticRecord& r) {
    // Hide the popup before emitting so the detail dialog doesn't
    // immediately steal focus and cause Qt::Popup to dismiss us
    // afterwards mid-animation.
    hide();
    emit diagnosticActivated(r);
  });
  connect(card, &DiagnosticsCard::copyRequested, this, [this, card](const DiagnosticRecord& r) {
    onCopyRequested(r, card);
  });
  ui_->cardsLayout->addWidget(card);
  cards_.push_back(card);
}

void DiagnosticsPopup::appendEmptyStateCard() {
  auto* placeholder = new QLabel(tr("No diagnostics"), ui_->scrollContents);
  placeholder->setObjectName(QStringLiteral("DiagnosticsEmptyState"));
  placeholder->setAlignment(Qt::AlignCenter);
  placeholder->setMinimumHeight(kCardHeight);
  ui_->cardsLayout->addWidget(placeholder);
}

void DiagnosticsPopup::onCopyRequested(const DiagnosticRecord& item, DiagnosticsCard* originating) {
  QGuiApplication::clipboard()->setText(item.message);
  auto* button = originating->findChild<QToolButton*>(QStringLiteral("cardCopyButton"));
  if (button == nullptr) {
    return;
  }
  const QIcon original = button->icon();
  button->setIcon(LoadSvg(":/resources/svg/check.svg", currentTheme()));
  QTimer::singleShot(250, button, [button, original]() { button->setIcon(original); });
}

int DiagnosticsPopup::sizedHeight() const {
  const int count = (history_ == nullptr) ? 0 : history_->size();
  const int visible = qMin(count, kMaxVisibleCards);
  const int rows = qMax(1, visible);  // empty-state row counts as 1
  return rows * kCardHeight + (rows - 1) * kCardSpacing + 2 * kFrameMargin + kDragHandleHeight;
}

int DiagnosticsPopup::minimumUserHeight() const {
  return kMaxVisibleCards * kCardHeight + (kMaxVisibleCards - 1) * kCardSpacing + 2 * kFrameMargin + kDragHandleHeight;
}

int DiagnosticsPopup::effectiveHeight() const {
  if (user_height_set_) {
    return std::max(user_height_, minimumUserHeight());
  }
  return sizedHeight();
}

void DiagnosticsPopup::applyEffectiveHeight() {
  QRect g = geometry();
  const int target = effectiveHeight();
  if (g.height() != target) {
    g.setHeight(target);
    setGeometry(g);
  }
}

void DiagnosticsPopup::showAt(QWidget* anchor) {
  if (anchor == nullptr) {
    qWarning("DiagnosticsPopup::showAt called with null anchor");
    return;
  }
  rebuildCards();
  // Fixed width — the bell button used to be 500 px wide and the popup
  // matched it; now the bell is a 23-px chrome button so we set the
  // popup width explicitly. The top-right of the popup is pinned to the
  // bottom-right of the anchor so the cards still fill the strip of
  // title bar that previously hosted the elided latest-log label.
  constexpr int kPopupWidth = 500;
  const int target_height = effectiveHeight();
  const QPoint anchor_bottom_right = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
  const QRect target(anchor_bottom_right.x() - kPopupWidth, anchor_bottom_right.y(), kPopupWidth, target_height);

  setGeometry(target.x(), target.y(), target.width(), 0);
  show();

  open_animation_->stop();
  open_animation_->setStartValue(QRect(target.x(), target.y(), target.width(), 0));
  open_animation_->setEndValue(target);
  open_animation_->start();
}

bool DiagnosticsPopup::eventFilter(QObject* watched, QEvent* event) {
  if (watched == ui_->scrollArea->viewport() && event->type() == QEvent::Resize) {
    syncContentsWidthToViewport();
    return false;
  }
  if (watched != ui_->dragHandle) {
    return QFrame::eventFilter(watched, event);
  }
  switch (event->type()) {
    case QEvent::Enter:
      hover_over_handle_ = true;
      setDragActive(true);
      break;
    case QEvent::Leave:
      hover_over_handle_ = false;
      // Keep the bottom border lit while a drag is in progress even if
      // the cursor wanders off the handle's hit zone.
      if (!dragging_) {
        setDragActive(false);
      }
      break;
    case QEvent::MouseButtonPress:
      beginHandleDrag(static_cast<QMouseEvent*>(event));
      return true;
    case QEvent::MouseMove:
      if (dragging_) {
        updateHandleDrag(static_cast<QMouseEvent*>(event));
        return true;
      }
      break;
    case QEvent::MouseButtonRelease:
      if (dragging_) {
        endHandleDrag(static_cast<QMouseEvent*>(event));
        return true;
      }
      break;
    default:
      break;
  }
  return QFrame::eventFilter(watched, event);
}

void DiagnosticsPopup::beginHandleDrag(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  // Stop the open animation so its end-value doesn't snap us back.
  open_animation_->stop();
  dragging_ = true;
  drag_start_global_y_ = event->globalPosition().toPoint().y();
  drag_start_height_ = height();
  // Suspend paints inside the scroll viewport for the duration of the
  // drag — N cards otherwise re-paint on every pixel of mouse-move as
  // the popup's setGeometry cascades through the layout. The popup
  // frame + drag handle keep updating normally since they live outside
  // scrollArea. One repaint at endHandleDrag flushes the final state.
  ui_->scrollArea->setUpdatesEnabled(false);
  setDragActive(true);
  event->accept();
}

void DiagnosticsPopup::updateHandleDrag(QMouseEvent* event) {
  const int delta = event->globalPosition().toPoint().y() - drag_start_global_y_;
  const int proposed = drag_start_height_ + delta;
  user_height_ = std::max(proposed, minimumUserHeight());
  user_height_set_ = true;
  QRect g = geometry();
  g.setHeight(user_height_);
  setGeometry(g);
  event->accept();
}

void DiagnosticsPopup::endHandleDrag(QMouseEvent* event) {
  dragging_ = false;
  ui_->scrollArea->setUpdatesEnabled(true);
  ui_->scrollArea->update();
  // Keep the purple bottom border only if the cursor is still over
  // the handle — otherwise revert to the neutral border colour.
  setDragActive(hover_over_handle_);
  event->accept();
}

void DiagnosticsPopup::setDragActive(bool active) {
  if (property("dragActive").toBool() == active) {
    return;
  }
  setProperty("dragActive", active);
  // QSS needs a manual nudge to re-evaluate the [dragActive="..."]
  // rule. unpolish/polish on this widget only — children are unaffected.
  style()->unpolish(this);
  style()->polish(this);
}

void DiagnosticsPopup::syncContentsWidthToViewport() {
  const int target_w = ui_->scrollArea->viewport()->width();
  if (target_w <= 0) {
    return;
  }
  if (ui_->scrollContents->width() != target_w || ui_->scrollContents->minimumWidth() != target_w ||
      ui_->scrollContents->maximumWidth() != target_w) {
    // setFixedWidth pins min == max == target so the cards' QHBoxLayout
    // minimumSizeHint can never expand scrollContents beyond it. The
    // scroll area's widgetResizable behaviour still resizes contents
    // on its own, but those resizes also clamp to the same value.
    ui_->scrollContents->setFixedWidth(target_w);
  }
}

}  // namespace PJ
