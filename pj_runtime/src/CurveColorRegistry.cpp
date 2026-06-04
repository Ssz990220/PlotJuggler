#include "pj_runtime/CurveColorRegistry.h"

namespace PJ {

std::optional<QString> CurveColorRegistry::color(const QString& curve_key) const {
  const auto it = colors_.find(curve_key);
  if (it == colors_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void CurveColorRegistry::setColor(const QString& curve_key, const QString& hex_color) {
  colors_[curve_key] = hex_color;
}

int CurveColorRegistry::nextPaletteIndex() {
  return next_palette_index_++;
}

void CurveColorRegistry::clear() {
  colors_.clear();
  next_palette_index_ = 0;
}

}  // namespace PJ
