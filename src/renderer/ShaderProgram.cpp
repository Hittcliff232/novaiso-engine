#include "renderer/ShaderProgram.h"

#include "core/FileIO.h"

#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <vector>

namespace novaiso::renderer {

namespace {

std::vector<ShaderProgram::Notification> g_shader_notifications;

void PushShaderNotification(std::string message, bool success) {
    g_shader_notifications.push_back({std::move(message), success});
}

bool CompileStage(GLuint stage, const std::string& source, const char* label) {
    const char* text = source.c_str();
    glShaderSource(stage, 1, &text, nullptr);
    glCompileShader(stage);
    GLint success = GL_FALSE;
    glGetShaderiv(stage, GL_COMPILE_STATUS, &success);
    if (success == GL_FALSE) {
        GLint length = 0;
        glGetShaderiv(stage, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length));
        glGetShaderInfoLog(stage, length, nullptr, log.data());
        std::cerr << "Failed to compile shader " << label << '\n' << log.data() << '\n';
        PushShaderNotification(std::string("Shader compile failed: ") + label + "\n" + log.data(), false);
        return false;
    }
    return true;
}

}  // namespace

ShaderProgram::~ShaderProgram() {
    Destroy();
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept {
    *this = std::move(other);
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        Destroy();
        id_ = other.id_;
        vertex_path_ = std::move(other.vertex_path_);
        fragment_path_ = std::move(other.fragment_path_);
        vertex_timestamp_ = other.vertex_timestamp_;
        fragment_timestamp_ = other.fragment_timestamp_;
        other.id_ = 0;
    }
    return *this;
}

bool ShaderProgram::Load(const std::filesystem::path& vertex_path, const std::filesystem::path& fragment_path) {
    vertex_path_ = vertex_path;
    fragment_path_ = fragment_path;
    return Compile();
}

bool ShaderProgram::ReloadIfChanged() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_reload_check_) {
        return false;
    }
    next_reload_check_ = now + std::chrono::milliseconds(250);
    if (!core::FileIO::Exists(vertex_path_) || !core::FileIO::Exists(fragment_path_)) {
        return false;
    }
    const auto vertex_time = core::FileIO::LastWriteTime(vertex_path_);
    const auto fragment_time = core::FileIO::LastWriteTime(fragment_path_);
    if (!vertex_time.has_value() || !fragment_time.has_value()) {
        return false;
    }
    if (*vertex_time != vertex_timestamp_ || *fragment_time != fragment_timestamp_) {
        return Compile();
    }
    return false;
}

void ShaderProgram::Bind() const {
    glUseProgram(id_);
}

void ShaderProgram::Destroy() {
    if (id_ != 0) {
        glDeleteProgram(id_);
        id_ = 0;
    }
}

void ShaderProgram::SetInt(const char* name, int value) const {
    glUniform1i(UniformLocation(name), value);
}

void ShaderProgram::SetFloat(const char* name, float value) const {
    glUniform1f(UniformLocation(name), value);
}

void ShaderProgram::SetVec2(const char* name, const glm::vec2& value) const {
    glUniform2f(UniformLocation(name), value.x, value.y);
}

void ShaderProgram::SetVec3(const char* name, const glm::vec3& value) const {
    glUniform3f(UniformLocation(name), value.x, value.y, value.z);
}

void ShaderProgram::SetVec4(const char* name, const glm::vec4& value) const {
    glUniform4f(UniformLocation(name), value.x, value.y, value.z, value.w);
}

void ShaderProgram::SetMat4(const char* name, const glm::mat4& value) const {
    glUniformMatrix4fv(UniformLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

GLuint ShaderProgram::Id() const {
    return id_;
}

std::vector<ShaderProgram::Notification> ShaderProgram::ConsumeNotifications() {
    std::vector<Notification> notifications = std::move(g_shader_notifications);
    g_shader_notifications.clear();
    return notifications;
}

bool ShaderProgram::Compile() {
    const std::string vertex_source = core::FileIO::ReadText(vertex_path_);
    const std::string fragment_source = core::FileIO::ReadText(fragment_path_);
    const auto vertex_time = core::FileIO::LastWriteTime(vertex_path_);
    const auto fragment_time = core::FileIO::LastWriteTime(fragment_path_);

    const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    if (!CompileStage(vertex, vertex_source, vertex_path_.string().c_str()) ||
        !CompileStage(fragment, fragment_source, fragment_path_.string().c_str())) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success == GL_FALSE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>(length));
        glGetProgramInfoLog(program, length, nullptr, log.data());
        std::cerr << "Failed to link program\n" << log.data() << '\n';
        PushShaderNotification(std::string("Shader link failed: ") + fragment_path_.string() + "\n" + log.data(), false);
        glDeleteProgram(program);
        return false;
    }

    Destroy();
    id_ = program;
    uniform_cache_.clear();
    vertex_timestamp_ = vertex_time.value_or(std::filesystem::file_time_type{});
    fragment_timestamp_ = fragment_time.value_or(std::filesystem::file_time_type{});
    next_reload_check_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    PushShaderNotification(std::string("Shaders compiled: ") + vertex_path_.filename().string() + " + " + fragment_path_.filename().string(), true);
    return true;
}

GLint ShaderProgram::UniformLocation(const char* name) const {
    const auto it = uniform_cache_.find(name);
    if (it != uniform_cache_.end()) {
        return it->second;
    }
    const GLint location = glGetUniformLocation(id_, name);
    uniform_cache_.emplace(name, location);
    return location;
}

}  // namespace novaiso::renderer
