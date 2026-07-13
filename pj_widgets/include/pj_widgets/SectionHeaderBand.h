#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace PJ {

class Search;

// The canonical section header band — the titlebar-tone strip that titles panel
// sections ("Grid", "Curve Width", …) and the FFT/plugin panels. The background
// comes from the host app's stylesheet via the class selector
// (PJ--SectionHeaderBand); the 2-px leading indent is baked here so every band
// reads the same without per-instance QSS.
//
// Height is the canonical `ChromeMetrics::bandHeight()`, so every band in the
// app reads at one height (the left-panel "Sources" header, etc.). It starts at the
// default-metrics height; the host keeps it in step by routing its
// `chromeMetricsChanged` broadcast to onChromeMetricsChanged (for plugin-loaded
// bands, MainWindow walks the panel and wires this after the .ui is inflated).
// Do NOT call setFixedHeight from outside — that reintroduces the per-instance
// divergence this widget exists to remove.
//
// Optional inline filter: set `filterPlaceholder` (in code or via the .ui
// property) to grow a trailing search-glyph + flat QLineEdit on the band — the
// same "filter banner" the left-panel "Datasets"/"Custom Series" headers show.
// The field is flat-styled by the `PJ--SectionHeaderBand QLineEdit` QSS rule, so
// it reads as part of the band, not a separate input. Give it `filterFieldName`
// so the dialog host can bind its textChanged to the plugin (the QLineEdit gets
// that objectName). Access the field via filterEdit().
class SectionHeaderBand : public QWidget {
  Q_OBJECT
  Q_PROPERTY(QString text READ text WRITE setText)
  Q_PROPERTY(QString filterPlaceholder READ filterPlaceholder WRITE setFilterPlaceholder)
  Q_PROPERTY(QString filterFieldName READ filterFieldName WRITE setFilterFieldName)
  Q_PROPERTY(QString trailingComboName READ trailingComboName WRITE setTrailingComboName)
  Q_PROPERTY(QString trailingButtonName READ trailingButtonName WRITE setTrailingButtonName)
  Q_PROPERTY(QString trailingButtonIcon READ trailingButtonIcon WRITE setTrailingButtonIcon)
 public:
  explicit SectionHeaderBand(const QString& title, QWidget* parent = nullptr);

  void setText(const QString& title);
  [[nodiscard]] QString text() const;

  // Enable/relabel the trailing inline filter. A non-empty placeholder grows the
  // search glyph + field on first use; empty leaves the band a plain title strip.
  void setFilterPlaceholder(const QString& placeholder);
  [[nodiscard]] QString filterPlaceholder() const;

  // objectName stamped on the filter QLineEdit so the dialog-host binding can
  // route its textChanged to the plugin. Safe to set before or after the field
  // exists. Defaults to "bandFilter".
  void setFilterFieldName(const QString& name);
  [[nodiscard]] QString filterFieldName() const;

  // The inline filter field's line edit, or nullptr until a placeholder enables
  // it. Backed by the canonical Search control (see filterSearch()).
  [[nodiscard]] QLineEdit* filterEdit() const;

  // The inline filter control, or nullptr until a placeholder enables it.
  [[nodiscard]] Search* filterSearch() const {
    return filter_search_;
  }

  // Dock a combo box on the RIGHT of the band, turning a "Section title" strip
  // into a compact "Section title ........ [dropdown]" header row. The band
  // creates the combo lazily and stamps `name` as its objectName, so the dialog
  // host populates/binds it exactly as it would a standalone .ui combo (found by
  // objectName). Intended as an alternative to placing the combo on its own line
  // below the band. Mutually exclusive with the inline filter in practice.
  void setTrailingComboName(const QString& name);
  [[nodiscard]] QString trailingComboName() const;

  // The docked combo, or nullptr until setTrailingComboName enables it.
  [[nodiscard]] QComboBox* trailingCombo() const {
    return trailing_combo_;
  }

  // Dock a flat, icon-only action button on the RIGHT of the band (e.g. an
  // "add folder" affordance beside the section title). `name` becomes the
  // button's objectName, so the dialog host wires its click exactly as it would
  // a standalone QPushButton (folder/file picker, etc.). `svgPath` is an SVG
  // resource path, theme-recoloured via loadSvg. Order-independent: set either
  // property first.
  void setTrailingButtonName(const QString& name);
  [[nodiscard]] QString trailingButtonName() const;
  void setTrailingButtonIcon(const QString& svgPath);
  [[nodiscard]] QString trailingButtonIcon() const;

  // The docked action button, or nullptr until one of the setters enables it.
  [[nodiscard]] QPushButton* trailingButton() const {
    return trailing_button_;
  }

  // Resize to the canonical band height for these metrics. Connect the host's
  // chromeMetricsChanged signal here so the band scales with the icon size.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private:
  // Grow the search glyph + filter field on first request (idempotent).
  void ensureFilter();
  // Create the right-docked combo on first request (idempotent).
  void ensureTrailingCombo();
  // Create the right-docked action button on first request (idempotent).
  void ensureTrailingButton();

  QHBoxLayout* layout_ = nullptr;
  QLabel* label_ = nullptr;
  Search* filter_search_ = nullptr;
  QString filter_field_name_ = QStringLiteral("bandFilter");
  // Last metrics seen, so a filter created after the host's broadcast still
  // sizes correctly (ensureFilter seeds the new Search from this).
  ChromeMetrics current_metrics_{};
  QComboBox* trailing_combo_ = nullptr;
  QString trailing_combo_name_;
  QPushButton* trailing_button_ = nullptr;
  QString trailing_button_name_;
  QString trailing_button_icon_;
};

}  // namespace PJ
