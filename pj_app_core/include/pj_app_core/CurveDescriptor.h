#pragma once

#include <QString>
#include <cstddef>

#include "pj_base/types.hpp"

namespace PJ {

struct CurveDescriptor {
  QString name;  // e.g. "/imu/orientation/x"
  TopicId topic_id;
  DatasetId dataset_id;
  std::size_t column_index;
  QString field_path;
  Timestamp display_offset_ns;
};

}  // namespace PJ
