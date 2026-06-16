// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_decode.h"

#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace pj::scene3d {

glm::mat4 poseToMat4(const PJ::sdk::Pose& pose) {
  const glm::mat4 t = glm::translate(
      glm::mat4(1.0F), glm::vec3(
                           static_cast<float>(pose.position.x), static_cast<float>(pose.position.y),
                           static_cast<float>(pose.position.z)));
  const glm::quat q(
      static_cast<float>(pose.orientation.w), static_cast<float>(pose.orientation.x),
      static_cast<float>(pose.orientation.y), static_cast<float>(pose.orientation.z));
  return t * glm::mat4_cast(glm::normalize(q));
}

glm::vec3 toVec3(const PJ::sdk::Point3& point) {
  return {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)};
}

glm::vec3 toVec3(const PJ::sdk::Vector3& vec) {
  return {static_cast<float>(vec.x), static_cast<float>(vec.y), static_cast<float>(vec.z)};
}

glm::vec4 toVec4(const PJ::sdk::ColorRGBA& color) {
  return {color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F};
}

namespace {

// Local model matrix for a solid primitive: pose-aligned, then non-uniformly
// scaled by its bounding extents (cube side lengths, sphere/ellipsoid diameters).
glm::mat4 solidModel(const PJ::sdk::Pose& pose, const PJ::sdk::Vector3& size) {
  return poseToMat4(pose) * glm::scale(glm::mat4(1.0F), toVec3(size));
}

// Intern a frame_id into `frames`, returning its index. Frames per batch are
// few (typically 1-2), so a linear scan beats a hash map here.
std::uint32_t internFrame(std::vector<std::string>& frames, const std::string& id) {
  for (std::uint32_t i = 0; i < frames.size(); ++i) {
    if (frames[i] == id) {
      return i;
    }
  }
  frames.push_back(id);
  return static_cast<std::uint32_t>(frames.size() - 1);
}

void decodeEntity(const PJ::sdk::SceneEntity& entity, DecodedSceneEntities& out) {
  const std::uint32_t frame = internFrame(out.frames, entity.frame_id);

  for (const auto& cube : entity.cubes) {
    out.cubes.push_back({solidModel(cube.pose, cube.size), toVec4(cube.color), frame});
  }

  for (const auto& sphere : entity.spheres) {
    out.spheres.push_back({solidModel(sphere.pose, sphere.size), toVec4(sphere.color), frame});
  }

  for (const auto& line : entity.lines) {
    MarkerLineBatch batch;
    batch.model = poseToMat4(line.pose);
    batch.color = toVec4(line.color);
    batch.thickness = static_cast<float>(line.thickness);
    batch.frame_index = frame;

    // Resolve the effective vertex order: explicit index buffer if present,
    // else 0..N-1.
    const std::size_t count = line.indices.empty() ? line.points.size() : line.indices.size();
    const std::size_t point_count = line.points.size();
    const bool per_vertex_color = line.colors.size() == line.points.size();
    // `indices` comes from the (untrusted) message: map a logical position to its
    // point index and reject out-of-range references instead of reading OOB.
    const auto resolve = [&](std::size_t i) { return line.indices.empty() ? i : line.indices[i]; };
    const auto point_at = [&](std::size_t idx) { return toVec3(line.points[idx]); };
    const auto color_at = [&](std::size_t idx) { return toVec4(line.colors[idx]); };

    // Expand topology into GL_LINES pairs, skipping any segment that references
    // an out-of-range vertex (per_vertex_color implies colors.size()==points.size()).
    const auto emit = [&](std::size_t a, std::size_t b) {
      const std::size_t ia = resolve(a);
      const std::size_t ib = resolve(b);
      if (ia >= point_count || ib >= point_count) {
        return;
      }
      batch.vertices.push_back(point_at(ia));
      batch.vertices.push_back(point_at(ib));
      if (per_vertex_color) {
        batch.colors.push_back(color_at(ia));
        batch.colors.push_back(color_at(ib));
      }
    };
    switch (line.type) {
      case PJ::sdk::LineType::kLineList:
        for (std::size_t i = 0; i + 1 < count; i += 2) {
          emit(i, i + 1);
        }
        break;
      case PJ::sdk::LineType::kLineStrip:
        for (std::size_t i = 0; i + 1 < count; ++i) {
          emit(i, i + 1);
        }
        break;
      case PJ::sdk::LineType::kLineLoop:
        for (std::size_t i = 0; i + 1 < count; ++i) {
          emit(i, i + 1);
        }
        if (count >= 2) {
          emit(count - 1, 0);
        }
        break;
    }
    if (!batch.vertices.empty()) {
      out.lines.push_back(std::move(batch));
    }
  }

  for (const auto& cyl : entity.cylinders) {
    out.cylinders.push_back(
        {solidModel(cyl.pose, cyl.size), toVec4(cyl.color), static_cast<float>(cyl.bottom_scale),
         static_cast<float>(cyl.top_scale), frame});
  }

  for (const auto& arrow : entity.arrows) {
    out.arrows.push_back(
        {poseToMat4(arrow.pose), toVec4(arrow.color), static_cast<float>(arrow.shaft_length),
         static_cast<float>(arrow.shaft_diameter), static_cast<float>(arrow.head_length),
         static_cast<float>(arrow.head_diameter), frame});
  }

  for (const auto& ax : entity.axes) {
    out.axes.push_back({poseToMat4(ax.pose), static_cast<float>(ax.length), static_cast<float>(ax.thickness), frame});
  }

  for (const auto& tri : entity.triangles) {
    MarkerTriangleBatch batch;
    batch.model = poseToMat4(tri.pose);
    batch.color = toVec4(tri.color);
    batch.frame_index = frame;

    const std::size_t count = tri.indices.empty() ? tri.points.size() : tri.indices.size();
    const std::size_t point_count = tri.points.size();
    const bool per_vertex_color = tri.colors.size() == tri.points.size();
    // `indices` is untrusted message data — resolve and bounds-check each vertex.
    const auto resolve = [&](std::size_t i) { return tri.indices.empty() ? i : tri.indices[i]; };
    const auto point_at = [&](std::size_t idx) { return toVec3(tri.points[idx]); };
    const auto color_at = [&](std::size_t idx) { return toVec4(tri.colors[idx]); };

    for (std::size_t i = 0; i + 2 < count; i += 3) {
      const std::size_t ia = resolve(i);
      const std::size_t ib = resolve(i + 1);
      const std::size_t ic = resolve(i + 2);
      if (ia >= point_count || ib >= point_count || ic >= point_count) {
        continue;  // out-of-range index in the message — skip this triangle
      }
      const glm::vec3 a = point_at(ia);
      const glm::vec3 b = point_at(ib);
      const glm::vec3 c = point_at(ic);
      // Flat normal, shared by the triple. Coincident/colinear points (valid input
      // from producers, e.g. degenerate padding triangles) give a zero cross, which
      // glm::normalize turns into NaN — that would poison the whole triangle in the
      // shader. Skip the degenerate triangle instead.
      const glm::vec3 cross = glm::cross(b - a, c - a);
      const float cross_len = glm::length(cross);
      if (cross_len < 1e-12F) {
        continue;
      }
      const glm::vec3 n = cross / cross_len;
      batch.vertices.insert(batch.vertices.end(), {a, b, c});
      batch.normals.insert(batch.normals.end(), {n, n, n});
      if (per_vertex_color) {
        batch.colors.insert(batch.colors.end(), {color_at(ia), color_at(ib), color_at(ic)});
      }
    }
    if (!batch.vertices.empty()) {
      out.triangles.push_back(std::move(batch));
    }
  }

  for (const auto& text : entity.texts) {
    if (text.text.empty()) {
      continue;
    }
    out.texts.push_back(
        {poseToMat4(text.pose), toVec4(text.color), text.text, static_cast<float>(text.font_size), text.billboard,
         frame});
  }
}

}  // namespace

DecodedSceneEntities decodeSceneEntities(const PJ::sdk::SceneEntities& batch) {
  DecodedSceneEntities out;
  for (const auto& entity : batch.entities) {
    decodeEntity(entity, out);
  }
  // Deletions are intentionally ignored: this is a stateless snapshot decoder.
  return out;
}

}  // namespace pj::scene3d
