// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/program.h"

#include <fmt/format.h>

#include <limits>
#include <string>
#include <utility>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {
namespace {

std::string trimNullTerminator(std::string log) {
  while (!log.empty() && log.back() == '\0') {
    log.pop_back();
  }
  return log;
}

template <typename Functions>
std::string shaderInfoLog(Functions& functions, GLuint shader) {
  GLint log_length = 0;
  functions.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
  if (log_length <= 1) {
    return {};
  }

  std::string log(static_cast<std::size_t>(log_length), '\0');
  GLsizei written = 0;
  functions.glGetShaderInfoLog(shader, log_length, &written, log.data());
  if (written > 0) {
    log.resize(static_cast<std::size_t>(written));
  }
  return trimNullTerminator(std::move(log));
}

template <typename Functions>
std::string programInfoLog(Functions& functions, GLuint program) {
  GLint log_length = 0;
  functions.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
  if (log_length <= 1) {
    return {};
  }

  std::string log(static_cast<std::size_t>(log_length), '\0');
  GLsizei written = 0;
  functions.glGetProgramInfoLog(program, log_length, &written, log.data());
  if (written > 0) {
    log.resize(static_cast<std::size_t>(written));
  }
  return trimNullTerminator(std::move(log));
}

template <typename Functions>
GLuint compileShader(
    Functions& functions, GLenum shader_type, std::string_view source, std::string_view label, std::string& error) {
  if (source.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
    error = fmt::format("{} shader source is too large", label);
    return 0U;
  }

  const GLuint shader = functions.glCreateShader(shader_type);
  if (shader == 0U) {
    error = fmt::format("Failed to create {} shader", label);
    return 0U;
  }

  const char* source_ptr = source.data();
  const GLint source_length = static_cast<GLint>(source.size());
  functions.glShaderSource(shader, 1, &source_ptr, &source_length);
  functions.glCompileShader(shader);

  GLint compile_status = GL_FALSE;
  functions.glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
  if (compile_status == GL_TRUE) {
    return shader;
  }

  const std::string log = shaderInfoLog(functions, shader);
  functions.glDeleteShader(shader);
  error = log.empty() ? fmt::format("{} shader compilation failed", label)
                      : fmt::format("{} shader compilation failed:\n{}", label, log);
  return 0U;
}

}  // namespace

Program::Program(GLuint id) : id_(id), owning_context_(QOpenGLContext::currentContext()) {}

Program::~Program() {
  if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
    withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteProgram(id_); });
  }
}

Program::Program(Program&& other) noexcept
    : id_(std::exchange(other.id_, 0U)),
      owning_context_(std::exchange(other.owning_context_, nullptr)),
      uniform_locations_(std::move(other.uniform_locations_)) {
  other.uniform_locations_.clear();
}

Program& Program::operator=(Program&& other) noexcept {
  if (this != &other) {
    if (id_ != 0U && deleteAllowedInCurrentContext(owning_context_)) {
      withGlFunctionsNoThrow([this](auto& functions) { functions.glDeleteProgram(id_); });
    }
    id_ = std::exchange(other.id_, 0U);
    owning_context_ = std::exchange(other.owning_context_, nullptr);
    uniform_locations_ = std::move(other.uniform_locations_);
    other.uniform_locations_.clear();
  }
  return *this;
}

Program::Result Program::fromSources(std::string_view vert_src, std::string_view frag_src) {
  return withGlFunctions([vert_src, frag_src](auto& functions) -> Program::Result {
    std::string error;
    const GLuint vert_shader = compileShader(functions, GL_VERTEX_SHADER, vert_src, "Vertex", error);
    if (vert_shader == 0U) {
      return error;
    }

    const GLuint frag_shader = compileShader(functions, GL_FRAGMENT_SHADER, frag_src, "Fragment", error);
    if (frag_shader == 0U) {
      functions.glDeleteShader(vert_shader);
      return error;
    }

    const GLuint program = functions.glCreateProgram();
    if (program == 0U) {
      functions.glDeleteShader(vert_shader);
      functions.glDeleteShader(frag_shader);
      return std::string{"Failed to create shader program"};
    }

    functions.glAttachShader(program, vert_shader);
    functions.glAttachShader(program, frag_shader);
    functions.glLinkProgram(program);

    GLint link_status = GL_FALSE;
    functions.glGetProgramiv(program, GL_LINK_STATUS, &link_status);

    functions.glDetachShader(program, vert_shader);
    functions.glDetachShader(program, frag_shader);
    functions.glDeleteShader(vert_shader);
    functions.glDeleteShader(frag_shader);

    if (link_status == GL_TRUE) {
      return Program{program};
    }

    const std::string log = programInfoLog(functions, program);
    functions.glDeleteProgram(program);
    if (log.empty()) {
      return std::string{"Shader program link failed"};
    }
    return fmt::format("Shader program link failed:\n{}", log);
  });
}

void Program::use() {
  withGlFunctions([this](auto& functions) { functions.glUseProgram(id_); });
}

GLuint Program::id() const noexcept {
  return id_;
}

GLint Program::uniformLocation(const char* name) {
  if (auto it = uniform_locations_.find(name); it != uniform_locations_.end()) {
    return it->second;
  }
  const GLint location =
      withGlFunctions([this, name](auto& functions) { return functions.glGetUniformLocation(id_, name); });
  uniform_locations_.emplace(name, location);
  return location;
}

void Program::setMat4(const char* name, const glm::mat4& m) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions(
      [location, &m](auto& functions) { functions.glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(m)); });
}

void Program::setMat3(const char* name, const glm::mat3& m) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions(
      [location, &m](auto& functions) { functions.glUniformMatrix3fv(location, 1, GL_FALSE, glm::value_ptr(m)); });
}

void Program::setVec3(const char* name, const glm::vec3& v) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions([location, &v](auto& functions) { functions.glUniform3fv(location, 1, glm::value_ptr(v)); });
}

void Program::setVec2Array(const char* name, const glm::vec2* values, int count) {
  const GLint location = uniformLocation(name);
  if (location < 0 || values == nullptr || count <= 0) {
    return;
  }
  withGlFunctions([location, values, count](auto& functions) {
    functions.glUniform2fv(location, count, glm::value_ptr(values[0]));
  });
}

void Program::setVec3Array(const char* name, const glm::vec3* values, int count) {
  const GLint location = uniformLocation(name);
  if (location < 0 || values == nullptr || count <= 0) {
    return;
  }
  withGlFunctions([location, values, count](auto& functions) {
    functions.glUniform3fv(location, count, glm::value_ptr(values[0]));
  });
}

void Program::setVec4(const char* name, const glm::vec4& v) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions([location, &v](auto& functions) { functions.glUniform4fv(location, 1, glm::value_ptr(v)); });
}

void Program::setFloat(const char* name, float v) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions([location, v](auto& functions) { functions.glUniform1f(location, v); });
}

void Program::setInt(const char* name, int v) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions([location, v](auto& functions) { functions.glUniform1i(location, v); });
}

void Program::setIVec3(const char* name, const glm::ivec3& v) {
  const GLint location = uniformLocation(name);
  if (location < 0) {
    return;
  }
  withGlFunctions([location, &v](auto& functions) { functions.glUniform3iv(location, 1, glm::value_ptr(v)); });
}

}  // namespace pj::scene3d::gl
