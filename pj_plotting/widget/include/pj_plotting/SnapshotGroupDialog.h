#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QStringList>
#include <cstddef>
#include <optional>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/Dialog.h"

class QComboBox;
class QListWidget;
class QLineEdit;
class QRadioButton;
class QCheckBox;
class QDoubleSpinBox;

namespace PJ {

class CatalogModel;

// Modal dialog to create a snapshot ("current message") curve group interactively:
// pick a topic, the array wildcard for X (element index or a leaf pattern), one or
// more Y leaf patterns, an alias prefix, and an optional manual y-range. Candidate
// wildcard patterns are enumerated from the topic's catalog columns
// (enumerateSnapshotPatterns). On accept the caller reads the selections and calls
// PlotWidget::addSnapshotCurveGroup + setFixedYRange.
class SnapshotGroupDialog : public Dialog {
  Q_OBJECT
 public:
  explicit SnapshotGroupDialog(CatalogModel* catalog, QWidget* parent = nullptr);
  ~SnapshotGroupDialog() override = default;

  [[nodiscard]] DatasetId datasetId() const noexcept {
    return dataset_id_;
  }
  [[nodiscard]] TopicId topicId() const noexcept {
    return topic_id_;
  }
  // The chosen X pattern; empty string means index x-mode.
  [[nodiscard]] QString xPattern() const;
  // The selected Y wildcard patterns (one curve each).
  [[nodiscard]] QStringList yPatterns() const;
  [[nodiscard]] QString aliasPrefix() const;
  [[nodiscard]] std::optional<double> yMin() const;
  [[nodiscard]] std::optional<double> yMax() const;

 private:
  void onTopicChanged();
  void refreshOkState();

  CatalogModel* catalog_ = nullptr;
  DatasetId dataset_id_ = 0;
  TopicId topic_id_ = 0;

  QComboBox* topic_combo_ = nullptr;
  QRadioButton* x_index_ = nullptr;
  QRadioButton* x_leaf_ = nullptr;
  QComboBox* x_combo_ = nullptr;
  QListWidget* y_list_ = nullptr;
  QLineEdit* alias_edit_ = nullptr;
  QCheckBox* auto_min_ = nullptr;
  QCheckBox* auto_max_ = nullptr;
  QDoubleSpinBox* min_spin_ = nullptr;
  QDoubleSpinBox* max_spin_ = nullptr;
  QWidget* ok_button_ = nullptr;

  // Per topic entry in the combo: (dataset, topic, topic_name).
  struct TopicEntry {
    DatasetId dataset_id = 0;
    TopicId topic_id = 0;
  };
  std::vector<TopicEntry> topics_;
};

}  // namespace PJ
