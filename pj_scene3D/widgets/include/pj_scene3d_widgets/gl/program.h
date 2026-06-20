#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QPointer>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace pj::scene3d::gl {

// Move-only RAII wrapper for one linked OpenGL shader program.
//
// The program is created (and the owning context recorded) inside fromSources();
// the destructor / move-assignment delete it only when the owning context (or a
// sharing one) is current — see gl::Buffer for the owning-context rationale.
class Program {
 public:
  using Result = std::variant<Program, std::string>;

  ~Program();

  Program(Program&& other) noexcept;
  Program& operator=(Program&& other) noexcept;

  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  [[nodiscard]] static Result fromSources(std::string_view vert_src, std::string_view frag_src);

  void use();
  [[nodiscard]] GLuint id() const noexcept;
  // Uniform location for `name`, cached per program after the first lookup
  // (locations are stable for a linked program, so the cache never needs
  // invalidation beyond move/destroy). Returns -1 for an unknown/inactive name.
  [[nodiscard]] GLint uniformLocation(const char* name);

  // All setXxx use glUniform* against the CURRENTLY BOUND program — call use()
  // first, or you write another program's uniforms.
  void setMat4(const char* name, const glm::mat4& m);
  void setMat3(const char* name, const glm::mat3& m);
  void setVec3(const char* name, const glm::vec3& v);
  void setVec2Array(const char* name, const glm::vec2* values, int count);
  void setVec3Array(const char* name, const glm::vec3* values, int count);
  void setVec4(const char* name, const glm::vec4& v);
  void setFloat(const char* name, float v);
  void setInt(const char* name, int v);
  void setIVec3(const char* name, const glm::ivec3& v);

 private:
  explicit Program(GLuint id);

  GLuint id_{0};
  QPointer<QOpenGLContext> owning_context_;
  std::unordered_map<std::string, GLint> uniform_locations_;
};

}  // namespace pj::scene3d::gl
