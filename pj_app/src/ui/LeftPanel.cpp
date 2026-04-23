#include "ui/LeftPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>

#include "pj_app_core/SvgUtil.h"
#include "ui_LeftPanel.h"

namespace PJ {

namespace {
constexpr const char* kHiddenFileKey = "MainWindow.hiddenFileFrame";
constexpr const char* kHiddenStreamingKey = "MainWindow.hiddenStreamingFrame";
constexpr const char* kHiddenPublishersKey = "MainWindow.hiddenPublishersFrame";
constexpr const char* kStreamingBufferKey = "MainWindow.streamingBufferValue";
}  // namespace

LeftPanel::LeftPanel(QWidget* parent) : QWidget(parent), ui_(new Ui::LeftPanel) {
  ui_->setupUi(this);

  QSettings settings;
  applyIcons(settings.value("StyleSheet::theme", "light").toString());

  // Placeholder streaming sources; wired to real plugins in Phase 1.
  ui_->comboStreaming->addItem(tr("ROS2 Topic Subscriber"));

  // Restore persisted collapse + buffer state. Keys match PJ3 verbatim so a
  // user's previous preferences carry across the version bump.
  const int buffer_value = settings.value(kStreamingBufferKey, 15).toInt();
  ui_->streamingSpinBox->setValue(buffer_value);

  loadCollapseStateFromSettings();

  connect(ui_->buttonHideFileFrame, &QPushButton::clicked, this,
          &LeftPanel::onHideFileFrameClicked);
  connect(ui_->buttonHideStreamingFrame, &QPushButton::clicked, this,
          &LeftPanel::onHideStreamingFrameClicked);
  connect(ui_->buttonHidePublishersFrame, &QPushButton::clicked, this,
          &LeftPanel::onHidePublishersFrameClicked);

  connect(ui_->buttonLoadDatafile, &QPushButton::clicked, this, &LeftPanel::loadDataRequested);
  connect(ui_->buttonReloadData, &QPushButton::clicked, this, &LeftPanel::reloadDataRequested);
  connect(ui_->buttonLoadLayout, &QPushButton::clicked, this, &LeftPanel::loadLayoutRequested);
  connect(ui_->buttonSaveLayout, &QPushButton::clicked, this, &LeftPanel::saveLayoutRequested);
  connect(ui_->checkBoxAddPrefix, &QCheckBox::toggled, this, &LeftPanel::addPrefixToggled);
  connect(ui_->checkBoxMergeData, &QCheckBox::toggled, this, &LeftPanel::mergeDataToggled);

  connect(ui_->buttonStreamingStart, &QPushButton::toggled, this,
          &LeftPanel::streamingStartToggled);
  connect(ui_->buttonStreamingPause, &QPushButton::toggled, this,
          &LeftPanel::streamingPauseToggled);
  connect(ui_->buttonStreamingOptions, &QPushButton::clicked, this,
          &LeftPanel::streamingOptionsRequested);
  connect(ui_->comboStreaming, &QComboBox::currentTextChanged, this,
          &LeftPanel::streamingSourceChanged);
  connect(ui_->streamingSpinBox, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int v) {
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
  const bool file_hidden = settings.value(kHiddenFileKey, false).toBool();
  const bool streaming_hidden = settings.value(kHiddenStreamingKey, false).toBool();
  const bool publishers_hidden = settings.value(kHiddenPublishersKey, false).toBool();

  ui_->frameFile->setHidden(file_hidden);
  ui_->buttonHideFileFrame->setText(file_hidden ? "+" : "-");
  ui_->frameStreaming->setHidden(streaming_hidden);
  ui_->buttonHideStreamingFrame->setText(streaming_hidden ? "+" : "-");
  ui_->framePublishers->setHidden(publishers_hidden);
  ui_->buttonHidePublishersFrame->setText(publishers_hidden ? "+" : "-");
}

void LeftPanel::onHideFileFrameClicked() {
  const bool was_hidden = ui_->frameFile->isHidden();
  ui_->frameFile->setHidden(!was_hidden);
  ui_->buttonHideFileFrame->setText(was_hidden ? "-" : "+");
  QSettings().setValue(kHiddenFileKey, !was_hidden);
}

void LeftPanel::onHideStreamingFrameClicked() {
  const bool was_hidden = ui_->frameStreaming->isHidden();
  ui_->frameStreaming->setHidden(!was_hidden);
  ui_->buttonHideStreamingFrame->setText(was_hidden ? "-" : "+");
  QSettings().setValue(kHiddenStreamingKey, !was_hidden);
}

void LeftPanel::onHidePublishersFrameClicked() {
  const bool was_hidden = ui_->framePublishers->isHidden();
  ui_->framePublishers->setHidden(!was_hidden);
  ui_->buttonHidePublishersFrame->setText(was_hidden ? "-" : "+");
  QSettings().setValue(kHiddenPublishersKey, !was_hidden);
}

}  // namespace PJ
