#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QToolButton>

namespace PJ {

/// A QToolButton that shows a themed SVG icon and re-tints itself automatically
/// when the application theme changes — with no per-button wiring by the host. It
/// replaces the hand-rolled "QToolButton + autoRaise + NoFocus + setFixedSize +
/// setIconSize + loadSvg + retint-on-stylesheetChanged" pattern that was repeated
/// across the app (and which several sites simply forgot, leaving icons stale on a
/// theme switch).
///
/// Sizing — two presets set BOTH the button extent and the icon size:
///   Size::kDefault → 24x24, Size::kSmaller → 20x20.
/// For buttons whose size tracks the Preferences "Icon size" chrome metric, call
/// setExtent(button_px, icon_px) instead and update it from the host's chrome-
/// metrics signal (the button extent there is icon_size + icon_padding).
///
/// State-dependent glyphs (a toggle that shows a different icon when checked, a
/// play/pause swap, a panel-open/closed chevron): call setIconPath(newPath) from
/// the state-change slot. The button stores the path and always renders it in the
/// CURRENT theme, so that one call both swaps the glyph AND keeps re-tinting it on
/// later theme switches.
///
/// Theme transparency: Qt 6 delivers theme changes as ApplicationPaletteChange /
/// StyleChange events (QApplication::paletteChanged was removed), so changeEvent is
/// the idiomatic zero-wiring hook — the host need only repaint qApp's palette /
/// stylesheet on a theme switch (which it already does) and every SvgButton updates.
class SvgButton : public QToolButton {
  Q_OBJECT
 public:
  /// Preset square sizes (button extent == icon size).
  enum class Size { kDefault, kSmaller };

  static constexpr int kDefaultExtent = 24;  // Size::kDefault
  static constexpr int kSmallerExtent = 20;  // Size::kSmaller

  explicit SvgButton(QWidget* parent = nullptr);
  /// Construct with an SVG resource path (":/resources/svg/...") at a size preset.
  explicit SvgButton(const QString& icon_path, Size size = Size::kDefault, QWidget* parent = nullptr);

  /// Set/replace the SVG resource path and render it at the current theme. Use this
  /// to swap a toggle's glyph by state — the re-tint on theme change is automatic.
  void setIconPath(const QString& icon_path);
  [[nodiscard]] const QString& iconPath() const noexcept {
    return icon_path_;
  }

  /// Apply a size preset (24x24 or 20x20).
  void setSize(Size size);

  /// Explicit pixel sizing for chrome-metrics-driven buttons: a square button of
  /// `button_px` holding an `icon_px` icon (presets use button_px == icon_px).
  void setExtent(int button_px, int icon_px);

 protected:
  void changeEvent(QEvent* event) override;

 private:
  /// (Re)load icon_path_ at the current theme onto the button. No-op if empty.
  void reloadIcon();

  QString icon_path_;
};

}  // namespace PJ
