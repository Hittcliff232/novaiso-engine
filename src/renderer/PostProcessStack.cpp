#include "renderer/PostProcessStack.h"

#include <array>
#include <chrono>
#include <stdexcept>

namespace novaiso::renderer {

namespace {

bool IsInternalNonPostEffect(std::string_view effect_name) {
    return effect_name == "sprite" ||
           effect_name == "light" ||
           effect_name == "wet_reflection" ||
           effect_name == "post";
}

}  // namespace

bool PostProcessStack::Initialize(const std::filesystem::path& shader_root) {
    shader_root_ = shader_root;

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    constexpr std::array<float, 24> quad = {
        -1.0f, -1.0f, 0.0f, 0.0f,
        1.0f, -1.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };
    glNamedBufferData(vbo_, sizeof(quad), quad.data(), GL_STATIC_DRAW);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(float) * 4);
    glEnableVertexArrayAttrib(vao_, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribFormat(vao_, 1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2);
    glVertexArrayAttribBinding(vao_, 0, 0);
    glVertexArrayAttribBinding(vao_, 1, 0);
    return true;
}

void PostProcessStack::Shutdown() {
    effects_.clear();
    ping_.Destroy();
    pong_.Destroy();
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
}

GLuint PostProcessStack::Process(GLuint input_texture, glm::ivec2 size, const std::vector<std::string>& effects, float time_seconds) {
    last_effect_timings_.clear();
    last_total_cpu_ms_ = 0.0f;
    if (effects.empty()) {
        return input_texture;
    }

    EnsureBuffers(size);

    const auto total_start = std::chrono::steady_clock::now();
    GLuint current_texture = input_texture;
    bool use_ping = true;
    for (const auto& effect_name : effects) {
        if (IsInternalNonPostEffect(effect_name)) {
            continue;
        }
        const auto effect_start = std::chrono::steady_clock::now();
        ShaderProgram& shader = EffectShader(effect_name);
        shader.ReloadIfChanged();

        Framebuffer& target = use_ping ? ping_ : pong_;
        target.Bind();
        glClear(GL_COLOR_BUFFER_BIT);
        DrawFullscreen(current_texture, shader, size, time_seconds);
        current_texture = target.Texture();
        use_ping = !use_ping;
        const auto effect_end = std::chrono::steady_clock::now();
        last_effect_timings_.push_back({
            effect_name,
            std::chrono::duration<float, std::milli>(effect_end - effect_start).count()
        });
    }

    last_total_cpu_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - total_start).count();
    Framebuffer::BindDefault(size);
    return current_texture;
}

void PostProcessStack::BlitToDefault(GLuint texture, glm::ivec2 size, float time_seconds) {
    Framebuffer::BindDefault(size);
    ShaderProgram& shader = EffectShader("copy");
    shader.ReloadIfChanged();
    DrawFullscreen(texture, shader, size, time_seconds);
}

void PostProcessStack::BlitToDefaultFitted(GLuint texture, glm::ivec2 source_size, glm::ivec2 target_size, float time_seconds) {
    Framebuffer::BindDefault(target_size);
    const float scale = std::min(
        static_cast<float>(target_size.x) / std::max(source_size.x, 1),
        static_cast<float>(target_size.y) / std::max(source_size.y, 1));
    const glm::ivec2 fitted_size{
        std::max(static_cast<int>(std::round(static_cast<float>(source_size.x) * scale)), 1),
        std::max(static_cast<int>(std::round(static_cast<float>(source_size.y) * scale)), 1)
    };
    const glm::ivec2 offset{
        std::max((target_size.x - fitted_size.x) / 2, 0),
        std::max((target_size.y - fitted_size.y) / 2, 0)
    };
    glViewport(offset.x, offset.y, fitted_size.x, fitted_size.y);
    ShaderProgram& shader = EffectShader("copy");
    shader.ReloadIfChanged();
    DrawFullscreen(texture, shader, source_size, time_seconds);
    glViewport(0, 0, target_size.x, target_size.y);
}

ShaderProgram& PostProcessStack::EffectShader(const std::string& effect_name) {
    if (IsInternalNonPostEffect(effect_name)) {
        return EffectShader("copy");
    }

    auto it = effects_.find(effect_name);
    if (it == effects_.end()) {
        ShaderProgram program;
        if (!program.Load(shader_root_ / "post.vert", shader_root_ / (effect_name + ".frag"))) {
            if (effect_name != "copy") {
                return EffectShader("copy");
            }
            throw std::runtime_error("Failed to load post effect shader: " + effect_name);
        }
        it = effects_.emplace(effect_name, std::move(program)).first;
    }
    return it->second;
}

void PostProcessStack::EnsureBuffers(glm::ivec2 size) {
    if (ping_.Size() != size) {
        ping_.Resize(size);
        pong_.Resize(size);
    }
}

void PostProcessStack::DrawFullscreen(GLuint texture, ShaderProgram& shader, glm::ivec2 size, float time_seconds) {
    shader.Bind();
    shader.SetInt("u_scene", 0);
    shader.SetVec2("u_texelSize", {1.0f / static_cast<float>(size.x), 1.0f / static_cast<float>(size.y)});
    shader.SetFloat("u_time", time_seconds);
    glBindTextureUnit(0, texture);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

const std::vector<PostProcessStack::EffectTiming>& PostProcessStack::LastEffectTimings() const {
    return last_effect_timings_;
}

float PostProcessStack::LastTotalCpuMs() const {
    return last_total_cpu_ms_;
}

}  // namespace novaiso::renderer
