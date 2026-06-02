// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DebugUi.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QKeySequence>
#include <QLatin1String>
#include <QMainWindow>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSettings>
#include <QShortcut>
#include <QWidget>
#include <functional>

#include "Theme.h"

namespace PJ {

namespace {

constexpr auto kQssSettingsKey = "DebugUi/qssLayerEnabled";

// Pesticide-style debug stylesheet appended over the active theme when
// the QSS layer is toggled on. Borders shift layout by 1px — accepted.
constexpr auto kDebugQss = R"qss(
* { border: 1px dotted #888888; }

QMainWindow      { border: 1px dashed #ff3b30; }
QDialog          { border: 1px dashed #ff9500; }
QWidget          { border: 1px dashed #ffd60a; }
QFrame           { border: 1px dashed #cccc00; }
QSplitter        { border: 1px dashed #34c759; }
QSplitter::handle{ border: 1px dashed #00b894; }
QTabWidget       { border: 1px dashed #00c7be; }
QTabWidget::pane { border: 1px dashed #5ac8fa; }
QTabBar::tab     { border: 1px dashed #007aff; }

QMenuBar         { border: 1px dashed #5856d6; }
QMenuBar::item   { border: 1px dashed #6c5ce7; }
QMenu            { border: 1px dashed #af52de; }
QMenu::item      { border: 1px dashed #d63384; }
QToolTip         { border: 1px dashed #ff2d92; }

QLineEdit        { border: 1px dashed #e91e63; }
QPlainTextEdit   { border: 1px dashed #f06292; }
QTextBrowser     { border: 1px dashed #ec407a; }
QComboBox        { border: 1px dashed #ab47bc; }
QAbstractSpinBox { border: 1px dashed #7e57c2; }
QCheckBox        { border: 1px dashed #5c6bc0; }
QRadioButton     { border: 1px dashed #42a5f5; }
QGroupBox        { border: 1px dashed #29b6f6; }
QLabel           { border: 1px dashed #26c6da; }

QPushButton,
QPushButton:hover,
QPushButton:pressed,
QPushButton:checked,
QPushButton:disabled,
QPushButton:checked:hover,
QPushButton:checked:disabled { border: 1px dashed #66bb6a; }

QListView        { border: 1px dashed #9ccc65; }
QTreeView        { border: 1px dashed #d4e157; }
QHeaderView::section { border: 1px dashed #ffee58; }

QScrollBar:horizontal,
QScrollBar:vertical { border: 1px dashed #ffa726; }
QScrollBar::handle:horizontal,
QScrollBar::handle:vertical { border: 1px dashed #ff7043; }
QSlider::groove:horizontal { border: 1px dashed #d84315; }
QSlider::handle:horizontal { border: 1px dashed #bf360c; }
QSlider::sub-page:horizontal { border: 1px dashed #8d6e63; }

PlotWidget       { border: 1px dashed #ef5350; }
QwtPlot          { border: 1px dashed #b71c1c; }

TitleBar         { border: 1px dashed #00bcd4; }
TitleBar QToolButton { border: 1px dashed #4dd0e1; }
)qss";

QColor colourForDepth(int depth) {
  return QColor::fromHsl((depth * 47) % 360, 220, 130, 220);
}

// Pesticide overlay: paints over the host's subtree, mouse-transparent,
// no layout impact. Repaints on geometry / show / hide events from any
// widget so nested moves keep the dashed outlines in sync.
class PesticideOverlay : public QWidget {
 public:
  explicit PesticideOverlay(QWidget* host) : QWidget(host), host_(host) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    hide();
  }

  void setEnabled(bool on) {
    if (enabled_ == on) {
      return;
    }
    enabled_ = on;
    if (enabled_) {
      qApp->installEventFilter(this);
      resizeToHost();
      raise();
      show();
      update();
    } else {
      qApp->removeEventFilter(this);
      hide();
    }
  }

  [[nodiscard]] bool isEnabled() const {
    return enabled_;
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!enabled_) {
      return false;
    }
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::Move:
      case QEvent::Show:
      case QEvent::Hide:
      case QEvent::ChildAdded:
      case QEvent::ChildRemoved:
      case QEvent::LayoutRequest:
        if (watched == host_) {
          resizeToHost();
        }
        update();
        break;
      default:
        break;
    }
    return false;
  }

  void paintEvent(QPaintEvent* /*event*/) override {
    if (!enabled_) {
      return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);

    std::function<void(QWidget*, int)> walk = [&](QWidget* widget, int depth) {
      if (widget == this || !widget->isVisible()) {
        return;
      }
      const QPoint top_left = widget->mapTo(host_, QPoint(0, 0));
      QRect rect(top_left, widget->size());
      rect.adjust(0, 0, -1, -1);
      painter.setPen(QPen(colourForDepth(depth), 1, Qt::DashLine));
      painter.drawRect(rect);
      for (QObject* child : widget->children()) {
        if (auto* child_widget = qobject_cast<QWidget*>(child)) {
          walk(child_widget, depth + 1);
        }
      }
    };
    walk(host_, 0);
  }

 private:
  void resizeToHost() {
    setGeometry(host_->rect());
    raise();
  }

  QWidget* host_;
  bool enabled_ = false;
};

}  // namespace

void DebugUi::installInto(QMainWindow* host, Theme* theme) {
  // Parented to the host so destruction is automatic.
  new DebugUi(host, theme);
}

DebugUi::DebugUi(QMainWindow* host, Theme* theme) : QObject(host), host_(host), theme_(theme) {
  overlay_ = new PesticideOverlay(host);
  qss_enabled_ = QSettings().value(kQssSettingsKey, false).toBool();

  auto* overlay_shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), host);
  overlay_shortcut->setContext(Qt::ApplicationShortcut);
  connect(overlay_shortcut, &QShortcut::activated, this, &DebugUi::toggleOverlay);

  auto* qss_shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q), host);
  qss_shortcut->setContext(Qt::ApplicationShortcut);
  connect(qss_shortcut, &QShortcut::activated, this, &DebugUi::toggleQssLayer);

  // Re-apply our debug layer whenever the base theme rewrites the global
  // stylesheet, so the overlay survives theme switches.
  if (theme_ != nullptr) {
    connect(theme_, &Theme::qssChanged, this, &DebugUi::applyQss);
  }

  // Restore persisted state.
  if (qss_enabled_) {
    applyQss();
  }
}

void DebugUi::toggleOverlay() {
  // overlay_ is a PesticideOverlay we constructed in the ctor; the type
  // is private to this translation unit so we stash it as QWidget* and
  // recover with static_cast. PesticideOverlay has no Q_OBJECT (it
  // doesn't need signals/slots) so qobject_cast wouldn't work anyway.
  auto* pesticide = static_cast<PesticideOverlay*>(overlay_);
  pesticide->setEnabled(!pesticide->isEnabled());
}

void DebugUi::toggleQssLayer() {
  qss_enabled_ = !qss_enabled_;
  QSettings().setValue(kQssSettingsKey, qss_enabled_);
  applyQss();
}

void DebugUi::applyQss() {
  // Theme owns the canonical stylesheet; we layer our debug rules on top
  // (or strip them off) by writing the global stylesheet directly. Order
  // of slot dispatch on qssChanged: Theme's own listener (which sets
  // qApp->styleSheet to expandedQss()) runs first because it was
  // connected during MainWindow construction; ours runs after.
  if (theme_ == nullptr) {
    return;
  }
  QString stylesheet = theme_->expandedQss();
  if (qss_enabled_) {
    stylesheet.append(QLatin1Char('\n'));
    stylesheet.append(QLatin1String(kDebugQss));
  }
  qApp->setStyleSheet(stylesheet);
}

}  // namespace PJ
