#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLFunctions_4_5_Core>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace pj::scene3d::gl {

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
  [[nodiscard]] GLint uniformLocation(const char* name);
  void setMat4(const char* name, const glm::mat4& m);
  void setMat3(const char* name, const glm::mat3& m);
  void setVec3(const char* name, const glm::vec3& v);
  // Upload `count` consecutive vec2/vec3 elements into a GLSL array uniform.
  // Like every setter here, requires use() first (glUniform*, not glProgramUniform*).
  void setVec2Array(const char* name, const glm::vec2* values, int count);
  void setVec3Array(const char* name, const glm::vec3* values, int count);
  void setVec4(const char* name, const glm::vec4& v);
  void setFloat(const char* name, float v);
  void setInt(const char* name, int v);

 private:
  explicit Program(GLuint id);

  GLuint id_{0};
};

}  // namespace pj::scene3d::gl
