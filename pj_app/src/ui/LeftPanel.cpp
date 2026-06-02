// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/LeftPanel.h"

#include <QAction>
#include <QComboBox>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QMargins>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <initializer_list>
#include <nlohmann/json.hpp>

#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_LeftPanel.h"

namespace PJ {

namespace {
constexpr const char* kRecentFilesKey = "File/recent";
// Streaming-buffer setting key — preserved verbatim from when the
// scrubber lived on the timeline so user-saved values survive the move.
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";
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

  // The cog is a one-shot Start trigger for the streaming source. There is
  // no stop affordance — the session lives until app shutdown or error.
  connect(ui_->buttonStreamingOptions, &QPushButton::clicked, this, &LeftPanel::streamingStartRequested);
  // Pause/resume of follow-live, independent of start/stop. PJ3 parity.
  connect(ui_->buttonStreamingPause, &QPushButton::toggled, this, &LeftPanel::streamingPauseToggled);
  connect(
      ui_->buttonStreamingPause, &QPushButton::toggled, this, [this](bool) { applyPauseButtonState(currentTheme()); });
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

void LeftPanel::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIcons(currentTheme());
}

void LeftPanel::setReloadEnabled(bool enabled) {
  ui_->buttonReloadData->setEnabled(enabled);
}

void LeftPanel::setRecentEnabled(bool enabled) {
  ui_->buttonRecentFiles->setEnabled(enabled);
}

void LeftPanel::setStreamingSources(const QStringList& names) {
  const QString previous = ui_->comboStreaming->currentText();
  {
    QSignalBlocker block(ui_->comboStreaming);
    ui_->comboStreaming->clear();
    ui_->comboStreaming->addItems(names);
    const int idx = ui_->comboStreaming->findText(previous);
    if (idx >= 0) {
      ui_->comboStreaming->setCurrentIndex(idx);
    }
  }
  // QComboBox auto-selects index 0 on the first addItems(), but the blocker
  // above swallows the corresponding currentTextChanged; emit once so the
  // visible selection matches what listeners (StreamingSourceManager) hold.
  const QString current = ui_->comboStreaming->currentText();
  if (current != previous) {
    emit streamingSourceChanged(current);
  }
}

void LeftPanel::applyPauseButtonState(QString theme) {
  const bool paused = ui_->buttonStreamingPause->isChecked();
  ui_->buttonStreamingPause->setIcon(
      LoadSvg(paused ? ":/resources/svg/play_arrow.svg" : ":/resources/svg/pause.svg", theme));
  ui_->buttonStreamingPause->setToolTip(paused ? tr("Resume streaming") : tr("Pause streaming"));
}

QDomElement LeftPanel::saveSourcesState(QDomDocument& doc) const {
  QDomElement element = doc.createElement(QStringLiteral("left_panel_state"));

  // sources_tab: report which of the three autoExclusive tabs is checked.
  if (ui_->tabFile->isChecked()) {
    element.setAttribute(QStringLiteral("sources_tab"), QStringLiteral("file"));
  } else if (ui_->tabStream->isChecked()) {
    element.setAttribute(QStringLiteral("sources_tab"), QStringLiteral("stream"));
  } else if (ui_->tabCloud->isChecked()) {
    element.setAttribute(QStringLiteral("sources_tab"), QStringLiteral("cloud"));
  }

  element.setAttribute(QStringLiteral("streaming_source"), ui_->comboStreaming->currentText());
  element.setAttribute(QStringLiteral("streaming_buffer"), QString::number(ui_->streamingSpinBox->value()));
  return element;
}

void LeftPanel::restoreSourcesState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("left_panel_state")) {
    return;
  }

  // sources_tab: setChecked(true) propagates via autoExclusive and the
  // connected lambdas (which switch the inputStack page). We deliberately
  // do NOT block these signals — switching the visible page is the
  // intended side-effect of selecting a tab.
  if (element.hasAttribute(QStringLiteral("sources_tab"))) {
    const QString tab = element.attribute(QStringLiteral("sources_tab"));
    if (tab == QStringLiteral("file")) {
      ui_->tabFile->setChecked(true);
    } else if (tab == QStringLiteral("stream")) {
      ui_->tabStream->setChecked(true);
    } else if (tab == QStringLiteral("cloud")) {
      ui_->tabCloud->setChecked(true);
    }
    // Unknown tab string -> silent no-op.
  }

  // streaming_source: pick the combo entry by display text. -1 from
  // findText means the source isn't currently in the combo (plugin
  // not installed) -> silent no-op. Block signals so we don't emit
  // streamingSourceChanged during restore.
  if (element.hasAttribute(QStringLiteral("streaming_source"))) {
    const QString src = element.attribute(QStringLiteral("streaming_source"));
    const int idx = ui_->comboStreaming->findText(src);
    if (idx >= 0) {
      const QSignalBlocker blocker(ui_->comboStreaming);
      ui_->comboStreaming->setCurrentIndex(idx);
    }
  }

  // streaming_buffer: setValue triggers the connected lambda which
  // writes QSettings AND emits streamingBufferChanged. Block signals
  // to suppress both.
  if (element.hasAttribute(QStringLiteral("streaming_buffer"))) {
    bool ok = false;
    const int seconds = element.attribute(QStringLiteral("streaming_buffer")).toInt(&ok);
    if (ok) {
      const QSignalBlocker blocker(ui_->streamingSpinBox);
      ui_->streamingSpinBox->setValue(seconds);
    }
  }
}

void LeftPanel::applyIcons(QString theme) {
  ui_->tabFile->setIcon(LoadSvg(":/resources/svg/draft.svg", theme));
  ui_->tabStream->setIcon(LoadSvg(":/resources/svg/cast.svg", theme));
  ui_->tabCloud->setIcon(LoadSvg(":/resources/svg/cloud.svg", theme));
  ui_->buttonLoadDatafile->setIcon(LoadSvg(":/resources/svg/upload_file.svg", theme));
  ui_->buttonReloadData->setIcon(LoadSvg(":/resources/svg/restore_page.svg", theme));
  ui_->buttonRecentFiles->setIcon(LoadSvg(":/resources/svg/play_arrow.svg", theme));
  ui_->buttonStreamingOptions->setIcon(LoadSvg(":/resources/svg/add_tab.svg", theme));
  applyPauseButtonState(theme);

  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  // Band height grows by 2 * layout_padding so the contentsMargins
  // applied to inner layouts are absorbed by the container instead of
  // squeezing the buttons.
  const int band_extent = button_extent + (2 * chrome_metrics_.layout_padding);
  // Every icon-bearing button in this panel uses the same square chrome
  // pattern; iterate by type rather than by name.
  for (auto* btn : findChildren<QToolButton*>()) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  for (auto* btn : findChildren<QPushButton*>()) {
    // Cloud-launcher buttons are runtime-added text buttons, not the square
    // icon chrome this loop styles — leave their natural sizing alone, else
    // they get squashed to button_extent x button_extent and clip their label.
    if (btn->objectName().startsWith(QStringLiteral("cloudToolboxOpen_"))) {
      continue;
    }
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  // Override the .ui-baked 24-px height pins on the input header band
  // and the streaming-row controls. Header bands grow to band_extent;
  // the inline streaming controls stay button-tall so they line up
  // visually with the buttons in their row.
  ui_->widgetLabelInput->setMinimumHeight(band_extent);
  ui_->widgetLabelInput->setMaximumHeight(band_extent);
  ui_->comboStreaming->setMinimumHeight(button_extent);
  ui_->comboStreaming->setMaximumHeight(button_extent);
  ui_->streamingSpinBox->setMinimumHeight(button_extent);
  ui_->streamingSpinBox->setMaximumHeight(button_extent);
  ui_->labelBuffer->setMinimumSize(button_extent, button_extent);
  ui_->labelBuffer->setMaximumSize(button_extent, button_extent);
  ui_->labelBuffer->setPixmap(RenderSvgPixmap(":/resources/svg/share_eta.svg", theme, icon_sz, devicePixelRatioF()));
  // Push layout_padding into every relevant layout — Sources header,
  // the file/stream/cloud page outer layouts, and the two streaming
  // rows. Spacing follows so individual items inside a row gain the
  // same breathing room as the band edges.
  const QMargins margins(
      chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
      chrome_metrics_.layout_padding);
  for (auto* layout : std::initializer_list<QLayout*>{
           ui_->inputHeaderLayout, ui_->pageFile->layout(), ui_->pageStream->layout(), ui_->pageCloud->layout(),
           ui_->streamSourceRow, ui_->streamBufferRow}) {
    if (layout != nullptr) {
      layout->setContentsMargins(margins);
      layout->setSpacing(chrome_metrics_.layout_spacing);
    }
  }
}

void LeftPanel::populateCloudToolboxes(const std::vector<RuntimeToolboxPlugin>& toolboxes) {
  auto* container = ui_->pageCloud->findChild<QWidget*>("cloudToolboxContainer");
  if (container == nullptr) {
    return;
  }
  auto* layout = qobject_cast<QVBoxLayout*>(container->layout());
  if (layout == nullptr) {
    return;
  }

  // Wipe existing rows.
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* w = item->widget()) {
      w->deleteLater();
    }
    delete item;
  }

  bool any_cloud = false;
  for (const auto& tb : toolboxes) {
    // The manifest lives on the loaded vtable as a constexpr char[]; a null
    // vtable is a load failure already reported through the diagnostic sink.
    const auto* vtable = tb.library.vtable();
    if (vtable == nullptr || vtable->manifest_json == nullptr) {
      continue;
    }
    auto manifest = nlohmann::json::parse(vtable->manifest_json, nullptr, /*allow_exceptions=*/false);
    if (!manifest.is_object()) {
      // The plugin loaded but ships a malformed manifest; warn so a toolbox that
      // silently never appears in the cloud list is diagnosable, then skip it.
      qWarning("LeftPanel: toolbox '%s' has an invalid manifest_json; skipping", tb.id.c_str());
      continue;
    }
    bool is_cloud = false;
    if (auto it = manifest.find("tags"); it != manifest.end() && it->is_array()) {
      for (const auto& tag : *it) {
        if (tag.is_string() && tag.get<std::string>() == "cloud") {
          is_cloud = true;
          break;
        }
      }
    }
    if (!is_cloud) {
      continue;
    }
    any_cloud = true;

    // One full-width button per cloud source; its label is the toolbox name
    // and clicking it launches the panel.
    auto* btn = new QPushButton(QString::fromStdString(tb.name), container);
    btn->setObjectName(QStringLiteral("cloudToolboxOpen_") + QString::fromStdString(tb.id));
    if (auto desc = manifest.find("description"); desc != manifest.end() && desc->is_string()) {
      btn->setToolTip(QString::fromStdString(desc->get<std::string>()));
    }
    layout->addWidget(btn);

    const QString id = QString::fromStdString(tb.id);
    connect(btn, &QPushButton::clicked, this, [this, id]() { emit cloudToolboxRequested(id); });
  }

  if (!any_cloud) {
    auto* placeholder = new QLabel(tr("(no cloud toolboxes installed)"), container);
    placeholder->setObjectName(QStringLiteral("labelCloudPlaceholder"));
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setEnabled(false);
    layout->addWidget(placeholder);
  }
  layout->addStretch();
}

}  // namespace PJ
