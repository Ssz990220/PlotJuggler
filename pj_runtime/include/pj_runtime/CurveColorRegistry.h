#pragma once

#include <QString>
#include <map>
#include <optional>

namespace PJ {

// Session-scoped memory of the color assigned to each curve, so a curve keeps
// its color when dragged into another plot (issue #68).
//
// Colors are stored as "#rrggbb" hex strings — the same form used in layout XML
// (QColor::name()) — so this service stays free of Qt6::Gui and pj_runtime keeps
// its minimal Qt surface. The widget layer (pj_plotting) converts to/from QColor
// at the boundary.
//
// Keyed by the opaque catalog key (CurveDescriptor::name) that PlotWidget::addCurve
// already receives. Lifetime is the session: clear() is called when the catalog is
// emptied (data cleared/replaced), matching PJ3's per-PlotData COLOR_HINT lifetime.
//
// Not thread-safe: all access happens on the GUI thread.
class CurveColorRegistry {
 public:
  // Returns the remembered hex color for `curve_key`, or nullopt if the curve has
  // not been assigned a color yet.
  [[nodiscard]] std::optional<QString> color(const QString& curve_key) const;

  // Remembers `hex_color` ("#rrggbb") for `curve_key`, overwriting any prior value.
  void setColor(const QString& curve_key, const QString& hex_color);

  // Hands out the next palette index (monotonic, shared across all plots) for a
  // brand-new curve that has no remembered color yet. Mirrors PJ3's app-wide
  // color counter so palette rotation stays consistent between plots.
  [[nodiscard]] int nextPaletteIndex();

  // Forgets every assignment and resets the palette counter to zero.
  void clear();

 private:
  std::map<QString, QString> colors_;
  int next_palette_index_ = 0;
};

}  // namespace PJ
