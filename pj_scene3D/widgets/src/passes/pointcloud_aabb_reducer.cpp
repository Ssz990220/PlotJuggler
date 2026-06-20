// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/pointcloud_aabb_reducer.h"

#include <QDebug>
#include <algorithm>
#include <array>
#include <string>

#include "pj_scene3d_core/aabb_gpu_key.h"
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/gl/program.h"

namespace pj::scene3d {
namespace {

constexpr GLuint kLocalSize = 256;
// Cap on workgroups: the shader grid-strides, so a few thousand groups cover any
// cloud while keeping per-workgroup atomic traffic (7 atomics each) bounded.
constexpr GLuint kMaxGroups = 1024;
// SSBO result slots: [0..2] min ordered key xyz, [3..5] max ordered key xyz,
// [6] finite-point count, [7] pad.
constexpr int kResultSlots = 8;
constexpr GLuint kSourceBinding = 0;
constexpr GLuint kResultBinding = 1;

// floatToOrderedKey() here MUST match pj_scene3d_core/aabb_gpu_key.h exactly.
// +inf / -inf seeds are spelled as bit patterns to avoid constant-fold issues
// with 1.0/0.0 on some GLSL compilers.
constexpr const char* kComputeSrc = R"(#version 430
layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer PointData { uint data[]; };
layout(std430, binding = 1) buffer Result { uint result[]; };

uniform uint u_point_count;
uniform uint u_stride_words;  // stride_bytes / 4
uniform uint u_x_word;        // x_offset_bytes / 4 ; y at +1, z at +2

uint floatToOrderedKey(float v) {
  uint bits = floatBitsToUint(v);
  uint mask = (bits & 0x80000000u) != 0u ? 0xFFFFFFFFu : 0x80000000u;
  return bits ^ mask;
}

bool isFiniteVec(vec3 p) {
  return !(isnan(p.x) || isnan(p.y) || isnan(p.z) || isinf(p.x) || isinf(p.y) || isinf(p.z));
}

shared vec3 s_lo[gl_WorkGroupSize.x];
shared vec3 s_hi[gl_WorkGroupSize.x];
shared uint s_fin[gl_WorkGroupSize.x];

void main() {
  vec3 lo = vec3(uintBitsToFloat(0x7F800000u));   // +inf
  vec3 hi = vec3(uintBitsToFloat(0xFF800000u));   // -inf
  uint fin = 0u;

  uint total = gl_NumWorkGroups.x * gl_WorkGroupSize.x;
  for (uint i = gl_GlobalInvocationID.x; i < u_point_count; i += total) {
    uint base = i * u_stride_words + u_x_word;
    vec3 p = vec3(uintBitsToFloat(data[base]),
                  uintBitsToFloat(data[base + 1u]),
                  uintBitsToFloat(data[base + 2u]));
    if (isFiniteVec(p)) {
      lo = min(lo, p);
      hi = max(hi, p);
      fin += 1u;
    }
  }

  uint lid = gl_LocalInvocationID.x;
  s_lo[lid] = lo;
  s_hi[lid] = hi;
  s_fin[lid] = fin;
  barrier();

  for (uint stride = gl_WorkGroupSize.x / 2u; stride > 0u; stride >>= 1u) {
    if (lid < stride) {
      s_lo[lid] = min(s_lo[lid], s_lo[lid + stride]);
      s_hi[lid] = max(s_hi[lid], s_hi[lid + stride]);
      s_fin[lid] += s_fin[lid + stride];
    }
    barrier();
  }

  if (lid == 0u && s_fin[0] > 0u) {
    atomicMin(result[0], floatToOrderedKey(s_lo[0].x));
    atomicMin(result[1], floatToOrderedKey(s_lo[0].y));
    atomicMin(result[2], floatToOrderedKey(s_lo[0].z));
    atomicMax(result[3], floatToOrderedKey(s_hi[0].x));
    atomicMax(result[4], floatToOrderedKey(s_hi[0].y));
    atomicMax(result[5], floatToOrderedKey(s_hi[0].z));
    atomicAdd(result[6], s_fin[0]);
  }
}
)";

}  // namespace

PointcloudAabbReducer::PointcloudAabbReducer() = default;

PointcloudAabbReducer::~PointcloudAabbReducer() {
  releaseGL();
}

bool PointcloudAabbReducer::ensureInitialized() {
  if (probed_) {
    return available_;  // probe is deterministic — never retry after the first attempt
  }
  probed_ = true;
  try {
    auto result = gl::Program::fromComputeSource(kComputeSrc);
    if (auto* program = std::get_if<gl::Program>(&result)) {
      program_ = std::make_unique<gl::Program>(std::move(*program));
      withCoreGlFunctions([this](auto& functions) {
        functions.glGenBuffers(1, &result_buffer_);
        functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, result_buffer_);
        functions.glBufferData(
            GL_SHADER_STORAGE_BUFFER, kResultSlots * static_cast<GLsizeiptr>(sizeof(GLuint)), nullptr, GL_DYNAMIC_DRAW);
        functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      });
      available_ = result_buffer_ != 0U;
    } else {
      qWarning().noquote() << "PointcloudAabbReducer: compute program unavailable, falling back to CPU bounds scan:\n"
                           << QString::fromStdString(std::get<std::string>(result));
      available_ = false;
    }
  } catch (const std::exception& ex) {
    qWarning() << "PointcloudAabbReducer init failed:" << ex.what();
    available_ = false;
  }
  return available_;
}

void PointcloudAabbReducer::dispatch(
    GLuint source_buffer, std::size_t point_count, uint32_t stride_bytes, uint32_t x_offset_bytes) {
  if (!ensureInitialized() || source_buffer == 0U || stride_bytes < 3 * kRequiredAlignmentBytes) {
    return;
  }
  if (pending_) {
    destroyFence();  // a previous result was never polled — latest-wins, discard it
  }

  const GLuint point_count_u = static_cast<GLuint>(std::min<std::size_t>(point_count, 0xFFFFFFFFull));
  const GLuint groups =
      std::max<GLuint>(1, std::min<GLuint>((point_count_u + kLocalSize - 1) / kLocalSize, kMaxGroups));
  // min slots seed to the LARGEST key (0xFFFFFFFF) so atomicMin reduces down;
  // max slots + count seed to 0 so atomicMax reduces up.
  const std::array<GLuint, kResultSlots> init = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u, 0u, 0u, 0u};

  withCoreGlFunctions([&](auto& functions) {
    functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, result_buffer_);
    functions.glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(init), init.data());
    functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSourceBinding, source_buffer);
    functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kResultBinding, result_buffer_);

    program_->use();
    program_->setUInt("u_point_count", point_count_u);
    program_->setUInt("u_stride_words", stride_bytes / kRequiredAlignmentBytes);
    program_->setUInt("u_x_word", x_offset_bytes / kRequiredAlignmentBytes);

    functions.glDispatchCompute(groups, 1, 1);
    functions.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    // Unbind so the source buffer is not held as an SSBO into the next frame's draws.
    functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSourceBinding, 0);
    functions.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kResultBinding, 0);
    functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    fence_ = functions.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  });
  pending_ = fence_ != nullptr;
}

std::optional<AABB> PointcloudAabbReducer::poll() {
  if (!pending_ || fence_ == nullptr) {
    return std::nullopt;
  }
  return withCoreGlFunctions([this](auto& functions) -> std::optional<AABB> {
    const GLenum wait = functions.glClientWaitSync(fence_, 0, 0);  // non-blocking poll
    if (wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED) {
      return std::nullopt;  // GPU still working — try again next frame
    }
    std::array<GLuint, kResultSlots> out{};
    functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, result_buffer_);
    functions.glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(out), out.data());
    functions.glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    destroyFence();
    pending_ = false;

    AABB box;
    if (out[6] == 0U) {
      box.valid = false;  // no finite points
      return box;
    }
    box.min = glm::vec3(orderedKeyToFloat(out[0]), orderedKeyToFloat(out[1]), orderedKeyToFloat(out[2]));
    box.max = glm::vec3(orderedKeyToFloat(out[3]), orderedKeyToFloat(out[4]), orderedKeyToFloat(out[5]));
    box.valid = true;
    return box;
  });
}

void PointcloudAabbReducer::destroyFence() {
  if (fence_ == nullptr) {
    return;
  }
  GLsync fence = fence_;
  fence_ = nullptr;
  withGlFunctionsNoThrow([fence](auto& functions) { functions.glDeleteSync(fence); });
}

void PointcloudAabbReducer::releaseGL() {
  destroyFence();
  pending_ = false;
  if (result_buffer_ != 0U) {
    GLuint buffer = result_buffer_;
    result_buffer_ = 0U;
    withGlFunctionsNoThrow([buffer](auto& functions) { functions.glDeleteBuffers(1, &buffer); });
  }
  program_.reset();  // gl::Program dtor deletes under the (still-current) dying context
  available_ = false;
  probed_ = false;  // re-probe lazily after a context recreation
}

}  // namespace pj::scene3d
