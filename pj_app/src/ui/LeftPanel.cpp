#include "ui/LeftPanel.h"

#include <QAction>
#include <QComboBox>
#include <QFileInfo>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QToolButton>

#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_LeftPanel.h"

namespace PJ {

namespace {
constexpr const char* kRecentFilesKey = "File/recent";
// Streaming-buffer setting key — preserved verbatim from when the
// scrubber lived on the timeline so user-saved values survive the move.
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";
constexpr QSize kStreamIconSize{20, 20};
}  // namespace

LeftPanel::LeftPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::LeftPanel) {
  ui_->setupUi(this);

  applyIcons(currentTheme());

  // Recent-files popup. Lazy-rebuilt on aboutToShow so it tracks
  // whatever MainWindow has pushed into QSettings("File/recent").
  auto* recent_menu = new QMenu(this);
  recent_menu->setObjectName(QStringLiteral("PJMenu"));
  connect(recent_menu, &QMenu::aboutToShow, this, [this, recent_menu]() {
    recent_menu->clear();
    const QStringList recent = QSettings().value(kRecentFilesKey).toStringList();
    if (recent.isEmpty()) {
      QAction* placeholder = recent_menu->addAction(tr("(no recent files)"));
      placeholder->setEnabled(false);
      return;
    }
    for (const QString& path : recent) {
      QAction* action = recent_menu->addAction(QFileInfo(path).fileName());
      action->setToolTip(path);
      connect(action, &QAction::triggered, this, [this, path]() { emit recentFileSelected(path); });
    }
  });
  connect(ui_->buttonRecentFiles, &QToolButton::clicked, this, [this, recent_menu]() {
    const QPoint anchor = ui_->buttonRecentFiles->mapToGlobal(QPoint(0, ui_->buttonRecentFiles->height()));
    recent_menu->popup(anchor);
  });

  connect(ui_->buttonLoadDatafile, &QPushButton::clicked, this, &LeftPanel::loadDataRequested);
  connect(ui_->buttonReloadData, &QPushButton::clicked, this, &LeftPanel::reloadDataRequested);

  // No data loaded yet -> nothing to reload, no recent entries.
  ui_->buttonReloadData->setEnabled(false);
  ui_->buttonRecentFiles->setEnabled(false);

  // Input section is tabbed: each toggle selects the matching page in
  // the stacked widget. autoExclusive=true on the .ui keeps only one
  // checked at a time, but we still drive the stack manually so the
  // check that fires on initial show also routes correctly.
  connect(ui_->tabFile, &QToolButton::toggled, this, [this](bool on) {
    if (on) {
      ui_->inputStack->setCurrentWidget(ui_->pageFile);
    }
  });
  connect(ui_->tabStream, &QToolButton::toggled, this, [this](bool on) {
    if (on) {
      ui_->inputStack->setCurrentWidget(ui_->pageStream);
    }
  });
  connect(ui_->tabCloud, &QToolButton::toggled, this, [this](bool on) {
    if (on) {
      ui_->inputStack->setCurrentWidget(ui_->pageCloud);
    }
  });

  // QStackedWidget's sizeHint is the union of every page's sizeHint, so
  // shorter pages still reserve room for the tallest one. Mark non-
  // current pages as Ignored so only the visible page contributes to
  // the parent's vertical sizing, and refresh on every page switch.
  auto adapt_stack_to_current_page = [this]() {
    QStackedWidget* stack = ui_->inputStack;
    const int current = stack->currentIndex();
    for (int i = 0; i < stack->count(); ++i) {
      QWidget* page = stack->widget(i);
      QSizePolicy policy = page->sizePolicy();
      policy.setVerticalPolicy(i == current ? QSizePolicy::Preferred : QSizePolicy::Ignored);
      page->setSizePolicy(policy);
    }
    stack->adjustSize();
  };
  adapt_stack_to_current_page();
  connect(ui_->inputStack, &QStackedWidget::currentChanged, this, [adapt_stack_to_current_page](int) {
    adapt_stack_to_current_page();
  });

  // The cog drives the start/stop toggle for the streaming source.
  connect(ui_->buttonStreamingOptions, &QPushButton::toggled, this, &LeftPanel::streamingStartToggled);
  connect(ui_->comboStreaming, &QComboBox::currentTextChanged, this, &LeftPanel::streamingSourceChanged);

  // Buffer scrubber: restore from QSettings on construct, persist + emit on change.
  ui_->streamingSpinBox->setValue(QSettings().value(kStreamingBufferKey, 5).toInt());
  connect(ui_->streamingSpinBox, &IntScrubber::valueChanged, this, [this](int seconds) {
    QSettings().setValue(kStreamingBufferKey, seconds);
    emit streamingBufferChanged(seconds);
  });
}

LeftPanel::~LeftPanel() {
  delete ui_;
}

void LeftPanel::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void LeftPanel::setReloadEnabled(bool enabled) {
  ui_->buttonReloadData->setEnabled(enabled);
}

void LeftPanel::setRecentEnabled(bool enabled) {
  ui_->buttonRecentFiles->setEnabled(enabled);
}

void LeftPanel::setStreamingSources(const QStringList& names) {
  const QString previous = ui_->comboStreaming->currentText();
  QSignalBlocker block(ui_->comboStreaming);
  ui_->comboStreaming->clear();
  ui_->comboStreaming->addItems(names);
  const int idx = ui_->comboStreaming->findText(previous);
  if (idx >= 0) {
    ui_->comboStreaming->setCurrentIndex(idx);
  }
}

void LeftPanel::applyIcons(QString theme) {
  ui_->tabFile->setIcon(LoadSvg(":/resources/svg/draft.svg", theme));
  ui_->tabStream->setIcon(LoadSvg(":/resources/svg/cast.svg", theme));
  ui_->tabCloud->setIcon(LoadSvg(":/resources/svg/cloud.svg", theme));
  ui_->buttonLoadDatafile->setIcon(LoadSvg(":/resources/svg/upload_file.svg", theme));
  ui_->buttonReloadData->setIcon(LoadSvg(":/resources/svg/restore_page.svg", theme));
  ui_->buttonRecentFiles->setIcon(LoadSvg(":/resources/svg/play_arrow.svg", theme));
  ui_->buttonStreamingOptions->setIcon(LoadSvg(":/resources/svg/tune.svg", theme));
  ui_->labelBuffer->setPixmap(
      RenderSvgPixmap(":/resources/svg/share_eta.svg", theme, kStreamIconSize, devicePixelRatioF()));
}

}  // namespace PJ
