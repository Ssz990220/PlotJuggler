// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDomElement>
#include <QString>
#include <cmath>
#include <initializer_list>

namespace pj::scene3d::detail {

// Layer payloads are attribute-only leaves. Unknown attributes are deliberately
// ignored for forward compatibility, while unknown nested elements remain a
// structural error because an older build cannot faithfully restore them.
inline bool isLeafPayload(const QDomElement& element) {
  for (QDomNode child = element.firstChild(); !child.isNull(); child = child.nextSibling()) {
    if (child.isElement()) {
      return false;
    }
    if ((child.isText() || child.isCDATASection()) && !child.nodeValue().trimmed().isEmpty()) {
      return false;
    }
  }
  return true;
}

inline bool parseFiniteFloat(
    const QDomElement& element, const char* attribute, float fallback, float minimum, float maximum, float& value) {
  bool ok = true;
  value = element.hasAttribute(QLatin1String(attribute)) ? element.attribute(QLatin1String(attribute)).toFloat(&ok)
                                                         : fallback;
  return ok && std::isfinite(value) && value >= minimum && value <= maximum;
}

inline bool parseTrueFalse(const QDomElement& element, const char* attribute, bool fallback, bool& value) {
  if (!element.hasAttribute(QLatin1String(attribute))) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(QLatin1String(attribute));
  if (text == QStringLiteral("true")) {
    value = true;
    return true;
  }
  if (text == QStringLiteral("false")) {
    value = false;
    return true;
  }
  return false;
}

inline bool parseZeroOne(const QDomElement& element, const char* attribute, bool fallback, bool& value) {
  if (!element.hasAttribute(QLatin1String(attribute))) {
    value = fallback;
    return true;
  }
  const QString text = element.attribute(QLatin1String(attribute));
  if (text == QStringLiteral("1")) {
    value = true;
    return true;
  }
  if (text == QStringLiteral("0")) {
    value = false;
    return true;
  }
  return false;
}

}  // namespace pj::scene3d::detail
