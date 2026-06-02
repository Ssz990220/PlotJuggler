#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFrame>
#include <QList>

#include "pj_runtime/DiagnosticHistory.h"

class QEvent;
class QMouseEvent;
class QPropertyAnimation;
class QToolButton;

namespace Ui {
class DiagnosticsPopup;
}

namespace PJ {

class DiagnosticsCard;

// Popup window listing recent diagnostics. Top-level Qt::Popup so an
// outside click dismisses instantly. Pure view onto a DiagnosticHistory
// service — the popup does not own the buffer; it observes the
// `recorded` signal and rebuilds cards when shown or live-updated.
//
// Sizing model:
//   - Auto-mode (default): popup height = min(records, 5) * card row.
//     Live appends grow the popup up to the five-card cap; beyond five,
//     the scroll area takes over and geometry stays put.
//   - User-mode: once the user drags the bottom handle, the popup adopts
//     a user-chosen height. Floor is the five-card height; subsequent
//     auto-appends do not change geometry. User-mode persists across
//     hide / show cycles within one session.
class DiagnosticsPopup : public QFrame {
  Q_OBJECT
 public:
  explicit DiagnosticsPopup(QWidget* parent = nullptr);
  ~DiagnosticsPopup() override;

  // Wire up the data source. Subscribes to `recorded` for live updates
  // and pulls an initial `snapshot()` so the popup is in sync from the
  // first show. May be called once; passing nullptr disconnects.
  void setHistory(DiagnosticHistory* history);

  // Anchors this popup to `anchor` (right edge aligned, top flush below
  // the anchor's bottom), rebuilds cards from history, then shows + runs
  // the open animation. Width is locked to anchor's current width.
  void showAt(QWidget* anchor);

  // Public for tests.
  [[nodiscard]] int cardCount() const;
  [[nodiscard]] bool isShowingEmptyState() const;

  static constexpr int kMaxVisibleCards = 5;
  static constexpr int kCardHeight = 28;
  static constexpr int kCardSpacing = 2;
  static constexpr int kFrameMargin = 4;
  static constexpr int kDragHandleHeight = 4;

 signals:
  void diagnosticActivated(const DiagnosticRecord& item);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void onRecorded(const DiagnosticRecord& r);
  void onHistoryCleared();

 private:
  void clearCards();
  void appendCard(const DiagnosticRecord& item);
  void appendEmptyStateCard();
  void onCopyRequested(const DiagnosticRecord& item, DiagnosticsCard* originating);
  void rebuildCards();
  // Auto-height for the current card count, capped at 5 cards.
  [[nodiscard]] int sizedHeight() const;
  // Height the popup should currently be — user-chosen if dragged, else auto.
  [[nodiscard]] int effectiveHeight() const;
  // Floor for any user-chosen height (5-card row).
  [[nodiscard]] int minimumUserHeight() const;
  // Apply effectiveHeight to current geometry without animation.
  void applyEffectiveHeight();

  // Drag handlers — driven by the eventFilter on dragHandle.
  void beginHandleDrag(QMouseEvent* event);
  void updateHandleDrag(QMouseEvent* event);
  void endHandleDrag(QMouseEvent* event);

  // Toggle the popup's "dragActive" dynamic property and re-polish so
  // QSS swaps the bottom border between border_default and purple.
  // True while the cursor is over dragHandle, or while a drag is in
  // progress (so the colour stays during a long drag that may cross
  // the handle's bounds).
  void setDragActive(bool active);

  // Pin scrollContents' width to the scroll area viewport's width so
  // the inner cards' minimumSizeHint can never push scrollContents
  // wider than the viewport. With horizontalScrollBarPolicy already
  // ScrollBarAlwaysOff, this guarantees no horizontal scroll range
  // can develop, no matter how the popup is resized / reopened.
  void syncContentsWidthToViewport();

  Ui::DiagnosticsPopup* ui_;
  DiagnosticHistory* history_ = nullptr;
  QList<DiagnosticsCard*> cards_;
  QPropertyAnimation* open_animation_ = nullptr;

  bool user_height_set_ = false;
  int user_height_ = 0;
  bool dragging_ = false;
  bool hover_over_handle_ = false;
  int drag_start_global_y_ = 0;
  int drag_start_height_ = 0;
};

}  // namespace PJ
