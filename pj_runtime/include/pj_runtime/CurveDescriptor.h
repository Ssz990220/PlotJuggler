#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <cstddef>

#include "pj_base/types.hpp"

namespace PJ {

struct CurveDescriptor {
  QString name;  // Opaque catalog key, not a display path.
  QString dataset_name;
  QString topic_name;
  QString field_name;
  TopicId topic_id;
  DatasetId dataset_id;
  std::size_t column_index;
  QString field_path;
  Timestamp display_offset_ns;
};

}  // namespace PJ
