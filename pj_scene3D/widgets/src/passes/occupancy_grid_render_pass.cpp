// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/passes/occupancy_grid_render_pass.h"

#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVersionFunctionsFactory>
#include <QString>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <utility>

#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcOccGridPass, "pj.scene3d.occupancy_grid_pass")

template <typename Callback>
void withGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return;
  }
  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    callback(*functions);
    return;
  }
  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    return;
  }
  functions->initializeOpenGLFunctions();
  callback(*functions);
}

glm::mat4 poseToMat4(const PJ::sdk::Pose& p) {
  const glm::quat q(
      static_cast<float>(p.orientation.w), static_cast<float>(p.orientation.x), static_cast<float>(p.orientation.y),
      static_cast<float>(p.orientation.z));
  const glm::mat4 rot = glm::mat4_cast(q);
  const glm::mat4 trans = glm::translate(
      glm::mat4(1.0f),
      glm::vec3(static_cast<float>(p.position.x), static_cast<float>(p.position.y), static_cast<float>(p.position.z)));
  return trans * rot;
}

// Unit quad in the local xy-plane: in_uv in [0,1]^2 doubles as position and
// texture coordinate (row-major cell (r,c) ↔ uv (c/width, r/height)).
constexpr float kQuad[] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};

constexpr const char* kVertSrc = R"(#version 450 core
layout(location = 0) in vec2 in_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
  v_uv = in_uv;
  gl_Position = u_mvp * vec4(in_uv, 0.0, 1.0);
}
)";

constexpr const char* kFragSrc = R"(#version 450 core
in vec2 v_uv;
uniform sampler2D u_grid;
uniform float u_opacity;
uniform int u_color_scheme;  // 0 = Map (grayscale), 1 = Costmap
out vec4 frag;
void main() {
  float raw = texture(u_grid, v_uv).r * 255.0;  // byte: 255 == unknown(-1); 0..100 occupancy %
  if (raw > 100.5) {
    discard;  // unknown / reserved → transparent
  }
  float occ = clamp(raw / 100.0, 0.0, 1.0);
  vec3 color;
  if (u_color_scheme == 1) {
    color = mix(vec3(0.0, 0.4, 1.0), vec3(1.0, 0.0, 0.0), occ);  // costmap: free→lethal
  } else {
    float g = 1.0 - occ;  // map: free=white, occupied=black
    color = vec3(g, g, g);
  }
  frag = vec4(color, u_opacity);
}
)";

}  // namespace

OccupancyGridRenderPass::OccupancyGridRenderPass() = default;
OccupancyGridRenderPass::~OccupancyGridRenderPass() = default;

void OccupancyGridRenderPass::initializeGL() {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  auto result = gl::Program::fromSources(kVertSrc, kFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result)) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    // Surface the GLSL failure instead of silently disabling the pass forever.
    // fromSources() returns an error string only when a context is current and
    // the shader genuinely failed to compile/link — a deterministic failure on
    // this driver — so initialized_ stays latched and we log exactly once rather
    // than re-compiling (and re-logging) on every frame.
    qCCritical(lcOccGridPass) << "OccupancyGrid shader build failed:"
                              << QString::fromStdString(std::get<std::string>(result))
                              << "— occupancy grids will not render on this GL context.";
    return;  // render() no-ops (program_ stays null)
  }
  vbo_.uploadStatic(GL_ARRAY_BUFFER, kQuad, sizeof(kQuad));
  withGlFunctions([this](auto& functions) {
    vao_.bind();
    vbo_.bind(GL_ARRAY_BUFFER);
    functions.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    functions.glEnableVertexAttribArray(0);
  });
}

void OccupancyGridRenderPass::setGrid(
    const ReconstructedGrid& grid, bool full_rebuild, const std::vector<CellRect>& dirty_rects) {
  const bool dims_changed = grid.width != width_ || grid.height != height_;
  frame_id_ = grid.frame_id;
  origin_ = grid.origin;
  resolution_ = grid.resolution;
  width_ = grid.width;
  height_ = grid.height;
  pending_cells_.resize(grid.cells.size());
  if (!grid.cells.empty()) {
    std::memcpy(pending_cells_.data(), grid.cells.data(), grid.cells.size());  // int8 → uint8, bit-identical
  }
  has_grid_ = width_ > 0 && height_ > 0 && !grid.cells.empty();

  // Full upload on a rebuild, when the texture isn't allocated, or when the dims
  // changed (texture must be reallocated). Otherwise patch only the dirty rects.
  if (full_rebuild || dims_changed || texture_.id() == 0U) {
    pending_full_ = true;
    pending_rects_.clear();
  } else if (!pending_full_) {
    pending_rects_.insert(pending_rects_.end(), dirty_rects.begin(), dirty_rects.end());
  }
  // else: a full upload is already staged but not yet consumed by render().
  // pending_cells_ (refreshed above) holds the latest full grid, so that staged
  // upload supersedes these rects — keep pending_full_ set. Downgrading it to an
  // incremental patch here would drop the rebuild and leave a stale base.
}

void OccupancyGridRenderPass::clearGrid() {
  has_grid_ = false;
}

void OccupancyGridRenderPass::releaseGL() {
  // Drop the program, quad VBO, VAO, and grid texture from the dying context.
  // pending_cells_ (the current full grid) is retained; re-arm a full upload so
  // render() repopulates the texture after initializeGL rebuilds the resources
  // in the new context.
  program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  texture_ = gl::Texture{};
  initialized_ = false;
  if (has_grid_) {
    pending_full_ = true;
    pending_rects_.clear();
  }
}

void OccupancyGridRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_ || !has_grid_) {
    return;
  }
  initializeGL();
  if (program_ == nullptr) {
    return;
  }
  if (pending_full_) {
    texture_.upload(width_, height_, pending_cells_.data());
    pending_full_ = false;
    pending_rects_.clear();
  } else if (!pending_rects_.empty()) {
    for (const CellRect& rect : pending_rects_) {
      // Pack the rect's cells tightly (the full grid's row stride is width_).
      std::vector<uint8_t> sub(static_cast<std::size_t>(rect.width) * rect.height);
      for (uint32_t row = 0; row < rect.height; ++row) {
        std::memcpy(
            sub.data() + static_cast<std::size_t>(row) * rect.width,
            pending_cells_.data() + (static_cast<std::size_t>(rect.y + row) * width_ + rect.x), rect.width);
      }
      texture_.uploadSub(
          static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y), rect.width, rect.height, sub.data());
    }
    pending_rects_.clear();
  }

  const auto transform = frame_ctx.lookup(frame_id_);
  if (!transform) {
    return;  // grid's frame can't resolve to the fixed frame — orphan, skip
  }
  const glm::mat4 t_fixed_from_frame = glm::mat4(transform->matrix());
  const glm::mat4 extent = glm::scale(
      glm::mat4(1.0f),
      glm::vec3(static_cast<float>(width_ * resolution_), static_cast<float>(height_ * resolution_), 1.0f));
  const glm::mat4 model = t_fixed_from_frame * poseToMat4(origin_) * extent;
  const glm::mat4 mvp = view_params.proj * view_params.view * model;

  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setFloat("u_opacity", opacity_);
  program_->setInt("u_color_scheme", static_cast<int>(color_scheme_));
  program_->setInt("u_grid", 0);
  texture_.bind(0);
  vao_.bind();

  withGlFunctions([](auto& functions) {
    functions.glEnable(GL_BLEND);
    functions.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    functions.glEnable(GL_POLYGON_OFFSET_FILL);
    functions.glPolygonOffset(-1.0f, -1.0f);  // pull toward camera to win over the ground/grid
    // Maps/costmaps are flat overlays, mutually coplanar at z=0. Writing depth
    // makes several grids z-fight in their overlap; instead keep the depth TEST
    // (so 3D geometry in front still occludes a grid) but disable depth WRITES,
    // so overlapping grids compose by draw order + alpha rather than fighting.
    functions.glDepthMask(GL_FALSE);
    functions.glDrawArrays(GL_TRIANGLES, 0, 6);
    functions.glDepthMask(GL_TRUE);
    functions.glDisable(GL_POLYGON_OFFSET_FILL);
    functions.glDisable(GL_BLEND);
  });
}

}  // namespace pj::scene3d
