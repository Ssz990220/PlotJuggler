#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSet>
#include <QString>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

class QHBoxLayout;
class QLineEdit;

namespace PJ {

class SvgButton;

/// The one canonical search/filter input across PJ4: a non-interactive
/// magnifying-glass glyph butted flush against a borderless line edit, both
/// sharing a single flat background so they read as one control. It replaces the
/// four hand-rolled variants that had drifted apart (the Datasets / Custom Series
/// / Curves "sibling search button + flat QLineEdit" and the marketplace
/// leading-action field).
///
/// The glyph is a themed SvgButton made mouse-transparent, so it self-retints on
/// a theme switch yet never steals a click. This bakes in the workaround every
/// prior site needed: Qt hardcodes a QLineEdit leading-action icon to 16 px on
/// fields shorter than 34 px, so a sibling button is the only way to get a
/// chrome-sized glyph. A trailing clear "x" appears only while the field holds
/// text.
///
/// Background: the widget paints a QSS-driven flat fill (WA_StyledBackground); the
/// child line edit and buttons are transparent, so the fill shows through as one
/// seamless surface. Two tones via the `variant` dynamic property —
/// Variant::kBanner (default, `banner_input` tone) for header-band filters, and
/// Variant::kStandalone (`input` tone) for a filter sitting on its own surface
/// (e.g. the marketplace).
///
/// Height tracks the app's icon-size chrome metric via setChromeMetrics(), so it
/// lines up with every section band; wire it to the host's chromeMetricsChanged
/// broadcast (MainWindow does this for band-hosted instances). Left unwired it
/// keeps the first-launch canonical height.
class Search : public QWidget {
  Q_OBJECT
  Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder)
  Q_PROPERTY(QString fieldObjectName READ fieldObjectName WRITE setFieldObjectName)
 public:
  /// Background tone (see the class doc). Selected via the `variant` dynamic
  /// property so the app stylesheet drives both fills.
  enum class Variant { kBanner, kStandalone };

  explicit Search(QWidget* parent = nullptr);
  ~Search() override;

  /// Placeholder shown when the field is empty. Defaults to "Filter...".
  void setPlaceholder(const QString& text);
  [[nodiscard]] QString placeholder() const;

  [[nodiscard]] QString text() const;
  void setText(const QString& text);
  void clear();

  /// The wrapped line edit — for advanced wiring (event filters, focus handling,
  /// returnPressed). Prefer text()/textChanged() for the common case.
  [[nodiscard]] QLineEdit* lineEdit() const {
    return line_edit_;
  }

  /// Stamp an objectName on the inner line edit so a dialog host can bind its
  /// textChanged by name (mirrors SectionHeaderBand::filterFieldName). Applying
  /// it to the inner field — not the Search frame — keeps existing per-name
  /// bindings working. Defaults to empty (the frame keeps its own objectName).
  void setFieldObjectName(const QString& name);
  [[nodiscard]] QString fieldObjectName() const;

  /// Select the background tone (see Variant). Default kBanner.
  void setVariant(Variant variant);
  [[nodiscard]] Variant variant() const {
    return variant_;
  }

  /// Size the control's height + glyphs to the app icon metrics (height ==
  /// icon_size + icon_padding), keeping it in step with the section bands.
  void setChromeMetrics(const ChromeMetrics& metrics);

 signals:
  /// Forwarded from the inner line edit on every text change.
  void textChanged(const QString& text);
  /// Forwarded when Return/Enter is pressed in the field.
  void returnPressed();

 protected:
  /// Tracks hover (across the frame + children) and the inner field's focus, so
  /// the border treatment applies to the WHOLE control rather than the line edit.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  /// Toggle the `pjHovered` / `pjFocused` dynamic property and repolish so the
  /// QSS border state updates. No-op when the state is unchanged.
  void setStateProperty(const char* name, bool on);

  QHBoxLayout* layout_ = nullptr;
  SvgButton* glyph_ = nullptr;
  SvgButton* clear_button_ = nullptr;
  QLineEdit* line_edit_ = nullptr;
  Variant variant_ = Variant::kBanner;
  ChromeMetrics chrome_metrics_{};
  /// The frame + child widgets currently under the mouse. Hover is the union
  /// (any non-empty) so moving between the glyph, field, and clear button reads
  /// as one continuous hover on the whole control.
  QSet<QObject*> hovered_objects_;
};

}  // namespace PJ
