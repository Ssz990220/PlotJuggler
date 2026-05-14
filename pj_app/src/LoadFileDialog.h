#pragma once

#include <QString>

#include "Dialog.h"

class QCheckBox;
class QFileSystemModel;
class QLineEdit;
class QPushButton;
class QTreeView;

namespace PJ {

// App-styled file picker for the data-load flow. Frameless Dialog
// chrome wraps a filesystem tree, a path readout, optional loader-flag
// checkboxes ("Add prefix" / "Merge metadata"), and OK/Cancel.
//
// Pass Options::NoOptions to hide the loader-flag checkboxes — useful
// for callers that just want a plain "open this file" picker (e.g.
// layout load).
class LoadFileDialog : public Dialog {
  Q_OBJECT
 public:
  enum class Options { ShowDataOptions, NoOptions };

  LoadFileDialog(
      QWidget* parent, const QString& start_dir, const QString& name_filter,
      Options options = Options::ShowDataOptions);
  ~LoadFileDialog() override;

  [[nodiscard]] QString selectedPath() const;
  [[nodiscard]] bool addPrefix() const;
  [[nodiscard]] bool mergeMetadata() const;

 private:
  void onSelectionChanged();

  QFileSystemModel* model_ = nullptr;
  QTreeView* tree_ = nullptr;
  QLineEdit* path_edit_ = nullptr;
  QCheckBox* add_prefix_ = nullptr;
  QCheckBox* merge_metadata_ = nullptr;
  QPushButton* ok_button_ = nullptr;
};

}  // namespace PJ
