#pragma once

namespace PJ {

// Contract for widgets that render object-store topics as layers (2D media today,
// 3D later), implemented in addition to IDataWidget. Lets the shell keep viewers
// coherent with object removal without knowing how many topics a viewer composes.
class IObjectViewer {
 public:
  virtual ~IObjectViewer() = default;

  // Pull-based re-validation against the live ObjectStore: drop layers whose topic
  // was evicted, keep the rest. Returns true if at least one live layer remains;
  // false when empty, in which case the shell resets the dock to the placeholder.
  virtual bool revalidateObjects() = 0;
};

}  // namespace PJ
