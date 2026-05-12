#include "ui/LeftPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <utility>

#include "pj_widgets/SvgUtil.h"
#include "ui_LeftPanel.h"

namespace PJ {

namespace {
constexpr const char* kHiddenFileKey = "MainWindow.hiddenFileFrame";
constexpr const char* kHiddenStreamingKey = "MainWindow.hiddenStreamingFrame";
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";
}  // namespace

LeftPanel::LeftPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::LeftPanel) {
  ui_->setupUi(this);

  applyIcons(currentTheme());
  QSettings settings;

  ui_->streamingSpinBox->setValue(settings.value(kStreamingBufferKey, 5).toInt());

  loadCollapseStateFromSettings();

  connect(ui_->buttonHideFileFrame, &QPushButton::clicked, this, [this]() {
    toggleSection(ui_->frameFile, ui_->buttonHideFileFrame, kHiddenFileKey);
  });
  connect(ui_->buttonHideStreamingFrame, &QPushButton::clicked, this, [this]() {
    toggleSection(ui_->frameStreaming, ui_->buttonHideStreamingFrame, kHiddenStreamingKey);
  });

  connect(ui_->buttonLoadDatafile, &QPushButton::clicked, this, &LeftPanel::loadDataRequested);
  connect(ui_->buttonReloadData, &QPushButton::clicked, this, &LeftPanel::reloadDataRequested);
  connect(ui_->buttonRecentData, &QPushButton::clicked, this, [this]() {
    const QPoint pos = ui_->buttonRecentData->mapToGlobal(ui_->buttonRecentData->rect().bottomLeft());
    emit recentDataRequested(pos);
  });
  connect(ui_->buttonLoadLayout, &QPushButton::clicked, this, &LeftPanel::loadLayoutRequested);
  connect(ui_->buttonSaveLayout, &QPushButton::clicked, this, &LeftPanel::saveLayoutRequested);
  connect(ui_->buttonRecentLayout, &QPushButton::clicked, this, [this]() {
    const QPoint pos = ui_->buttonRecentLayout->mapToGlobal(ui_->buttonRecentLayout->rect().bottomLeft());
    emit recentLayoutRequested(pos);
  });

  // No data loaded yet -> nothing to reload, no recent entries.
  ui_->buttonReloadData->setEnabled(false);
  ui_->buttonRecentData->setEnabled(false);
  ui_->buttonRecentLayout->setEnabled(false);
  connect(ui_->checkBoxAddPrefix, &QCheckBox::toggled, this, &LeftPanel::addPrefixToggled);
  connect(ui_->checkBoxMergeData, &QCheckBox::toggled, this, &LeftPanel::mergeDataToggled);

  connect(ui_->buttonStreamingStart, &QPushButton::toggled, this, &LeftPanel::streamingStartToggled);
  connect(ui_->buttonStreamingPause, &QPushButton::toggled, this, &LeftPanel::streamingPauseToggled);
  connect(ui_->buttonStreamingOptions, &QPushButton::clicked, this, &LeftPanel::streamingOptionsRequested);
  connect(ui_->comboStreaming, &QComboBox::currentTextChanged, this, &LeftPanel::streamingSourceChanged);
  connect(ui_->streamingSpinBox, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
    QSettings().setValue(kStreamingBufferKey, v);
    emit streamingBufferChanged(v);
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
  ui_->buttonRecentData->setEnabled(enabled);
}

void LeftPanel::setRecentLayoutEnabled(bool enabled) {
  ui_->buttonRecentLayout->setEnabled(enabled);
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
  ui_->buttonLoadDatafile->setIcon(LoadSvg(":/resources/svg/import.svg", theme));
  ui_->buttonReloadData->setIcon(LoadSvg(":/resources/svg/reload.svg", theme));
  ui_->buttonRecentData->setIcon(LoadSvg(":/resources/svg/play_arrow.svg", theme));
  ui_->buttonLoadLayout->setIcon(LoadSvg(":/resources/svg/import.svg", theme));
  ui_->buttonSaveLayout->setIcon(LoadSvg(":/resources/svg/export.svg", theme));
  ui_->buttonRecentLayout->setIcon(LoadSvg(":/resources/svg/play_arrow.svg", theme));
  ui_->buttonStreamingOptions->setIcon(LoadSvg(":/resources/svg/settings_cog.svg", theme));
  ui_->buttonStreamingPause->setIcon(LoadSvg(":/resources/svg/pause.svg", theme));
  ui_->buttonStreamingNotifications->setIcon(LoadSvg(":/resources/svg/alarm-bell.svg", theme));
}

void LeftPanel::loadCollapseStateFromSettings() {
  QSettings settings;
  const std::pair<QFrame*, std::pair<QPushButton*, const char*>> sections[] = {
      {ui_->frameFile, {ui_->buttonHideFileFrame, kHiddenFileKey}},
      {ui_->frameStreaming, {ui_->buttonHideStreamingFrame, kHiddenStreamingKey}},
  };
  for (const auto& [frame, button_and_key] : sections) {
    const bool hidden = settings.value(button_and_key.second, false).toBool();
    frame->setHidden(hidden);
    button_and_key.first->setText(hidden ? "+" : "-");
  }
}

void LeftPanel::toggleSection(QFrame* frame, QPushButton* button, const char* settings_key) {
  const bool new_hidden = !frame->isHidden();
  frame->setHidden(new_hidden);
  button->setText(new_hidden ? "+" : "-");
  QSettings().setValue(settings_key, new_hidden);
}

}  // namespace PJ
