#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

namespace detail {

// Append one path segment to `base`, normalized for display: dots become '/',
// leading slashes are stripped, and a single '/' separator is inserted between
// non-empty segments. An empty segment is a no-op.
inline void appendDisplayPath(QString& base, QString path) {
  path.replace('.', '/');
  while (path.startsWith('/')) {
    path.remove(0, 1);
  }
  if (path.isEmpty()) {
    return;
  }
  if (!base.isEmpty() && !base.endsWith('/')) {
    base += '/';
  }
  base += path;
}

}  // namespace detail

// The full human-readable path used to label a curve: "topic/field" (dots
// normalized to '/', leading slashes stripped), falling back to the opaque catalog
// key when both topic and field are empty. This is the single formatter the UI
// uses so a curve reads identically everywhere it appears — the plot legend, the
// Filter Editor's source list, and its before/after preview. The topic prefix must
// be kept: a bare field like "/twist/twist/linear/x" is ambiguous across topics
// (e.g. odom vs cmd_vel).
inline QString curveDisplayName(const CurveDescriptor& descriptor) {
  QString name;
  detail::appendDisplayPath(name, descriptor.topic_name);
  detail::appendDisplayPath(name, descriptor.field_name);
  return name.isEmpty() ? descriptor.name : name;
}

}  // namespace PJ
