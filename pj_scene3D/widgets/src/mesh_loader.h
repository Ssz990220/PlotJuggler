#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// CPU-side async mesh loader (design §B.4 + §3 embedded path). Imports STL /
// Collada(DAE) / OBJ / glTF via assimp on a worker thread (QtConcurrent::run)
// and returns a QFuture<MeshData> of interleaved vertices + indices. There is
// NO GL here: the render pass (Prompt 3) owns buffer upload. Results are cached
// by resolved path so the same mesh referenced by multiple links loads once.

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QMutex>
#include <QString>

#include "pj_scene3d_widgets/mesh_data.h"

namespace pj::scene3d {

// Stateless-ish loader with a path-keyed cache. Holds no GL state and is safe to
// keep on the entity. Both entry points run assimp on a QtConcurrent thread and
// hand back a QFuture; a test may call .result() to block.
class MeshLoader {
 public:
  MeshLoader() = default;

  // Load from a resolved filesystem path. Returns a cached future when the same
  // path was loaded before. assimp infers the format from the file extension.
  QFuture<MeshData> load(const QString& resolved_path);

  // Load from an in-memory buffer (embedded glTF / Waymo path, design §3). The
  // `format_hint` is an assimp extension hint without the dot (e.g. "glb",
  // "gltf", "dae") so assimp can pick the importer for headerless buffers.
  QFuture<MeshData> loadFromMemory(const QByteArray& bytes, const QString& format_hint);

  // Drop all cached results (e.g. when a dataset is swapped out).
  void clearCache();

  // Synchronous workers — exposed for unit tests and reused by the async paths.
  // `flip_to_z_up` applies the explicit Y-up -> Z-up (+90deg about X) into our
  // Z-up world. Needed for glTF (Y-up by spec) and Collada (assimp normalizes
  // its <up_axis> to assimp-internal Y-up); STL/OBJ pass through, so false.
  static MeshData importFromFile(const QString& path, bool flip_to_z_up);
  static MeshData importFromMemory(const QByteArray& bytes, const QString& format_hint, bool flip_to_z_up);

 private:
  // True for formats assimp delivers in Y-up (glTF by spec, Collada via its
  // <up_axis> normalization), which then need the explicit Y->Z flip.
  static bool wantsZUpFlip(const QString& format_hint);

  // Path-keyed cache of in-flight and completed loads. Mutex-guarded: the
  // entry points are called from the GUI thread while earlier loads still
  // run on QtConcurrent workers.
  QMutex cache_mutex_;
  QHash<QString, QFuture<MeshData>> cache_;
};

}  // namespace pj::scene3d
