// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotScaleDraw.h"

#include <qwt_text.h>

#include <QLocale>

namespace PJ {

QwtText PlotScaleDraw::label(double value) const {
  const QLocale locale;
  QString str = locale.toString(value, 'f', 6);
  // Strip trailing fractional zeros, then a bare decimal separator — using the
  // locale's own digits/separator so decimal-comma locales trim correctly. The
  // 'f' format guarantees a separator, which shields the integer digits.
  const QString zero = locale.zeroDigit();
  const QString point = locale.decimalPoint();
  while (str.endsWith(zero)) {
    str.chop(zero.size());
  }
  if (str.endsWith(point)) {
    str.chop(point.size());
  }
  return str;
}

}  // namespace PJ
