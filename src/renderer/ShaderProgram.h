#pragma once

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace novaiso::renderer {

class ShaderProgram {
public:
    struct Notification {
        std::string message;
        bool success = false;
    };

    ShaderProgram() = default;
    ~ShaderProgram();

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    bool Load(const std::filesystem::path& vertex_path, const std::filesystem::path& fragment_path);
    bool ReloadIfChanged();
    void Bind() const;
    void Destroy();

    void SetInt(const char* name, int value) const;
    void SetFloat(const char* name, float value) const;
    void SetVec2(const char* name, const glm::vec2& value) const;
    void SetVec3(const char* name, const glm::vec3& value) const;
    void SetVec4(const char* name, const glm::vec4& value) const;
    void SetMat4(const char* name, const glm::mat4& value) const;

    [[nodiscard]] GLuint Id() const;
    static std::vector<Notification> ConsumeNotifications();

private:
    bool Compile();
    GLint UniformLocation(const char* name) const;

    GLuint id_ = 0;
    std::filesystem::path vertex_path_;
    std::filesystem::path fragment_path_;
    std::filesystem::file_time_type vertex_timestamp_{};
    std::filesystem::file_time_type fragment_timestamp_{};
    mutable std::unordered_map<std::string, GLint> uniform_cache_;
    std::chrono::steady_clock::time_point next_reload_check_{};
};

}  // namespace novaiso::renderer
