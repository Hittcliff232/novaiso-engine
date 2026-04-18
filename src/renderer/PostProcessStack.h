#pragma once

#include "renderer/Framebuffer.h"
#include "renderer/ShaderProgram.h"

#include <glad/gl.h>
#include <glm/vec2.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace novaiso::renderer {

class PostProcessStack {
public:
    struct EffectTiming {
        std::string name;
        float cpu_ms = 0.0f;
    };

    bool Initialize(const std::filesystem::path& shader_root);
    void Shutdown();
    GLuint Process(GLuint input_texture, glm::ivec2 size, const std::vector<std::string>& effects, float time_seconds);
    void BlitToDefault(GLuint texture, glm::ivec2 size, float time_seconds);
    void BlitToDefaultFitted(GLuint texture, glm::ivec2 source_size, glm::ivec2 target_size, float time_seconds);
    [[nodiscard]] const std::vector<EffectTiming>& LastEffectTimings() const;
    [[nodiscard]] float LastTotalCpuMs() const;

private:
    ShaderProgram& EffectShader(const std::string& effect_name);
    void EnsureBuffers(glm::ivec2 size);
    void DrawFullscreen(GLuint texture, ShaderProgram& shader, glm::ivec2 size, float time_seconds);

    std::filesystem::path shader_root_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    Framebuffer ping_;
    Framebuffer pong_;
    std::unordered_map<std::string, ShaderProgram> effects_;
    std::vector<EffectTiming> last_effect_timings_;
    float last_total_cpu_ms_ = 0.0f;
};

}  // namespace novaiso::renderer
