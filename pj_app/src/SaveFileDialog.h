#pragma once

#include <QString>

#include "Dialog.h"

class QFileSystemModel;
class QLineEdit;
class QPushButton;
class QTreeView;

namespace PJ {

// App-styled save-file picker. Frameless Dialog chrome wraps a
// filesystem tree (filtered to the requested name pattern), an editable
// filename field, and Save/Cancel buttons. The default extension is
// auto-appended on accept if the user-typed name lacks it.
class SaveFileDialog : public Dialog {
  Q_OBJECT
 public:
  SaveFileDialog(
      QWidget* parent, const QString& start_dir, const QString& name_filter, const QString& default_extension);
  ~SaveFileDialog() override;

  // Absolute path the user accepted (with default_extension applied if
  // the typed name didn't already end with it). Empty if rejected.
  [[nodiscard]] QString selectedPath() const;

 private:
  void onSelectionChanged();
  void onFilenameChanged();
  void onAccept();

  QFileSystemModel* model_ = nullptr;
  QTreeView* tree_ = nullptr;
  QLineEdit* filename_edit_ = nullptr;
  QPushButton* save_button_ = nullptr;
  QString default_extension_;
  QString resolved_path_;
};

}  // namespace PJ
