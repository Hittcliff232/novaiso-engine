#pragma once

#include "camera/Camera2D.h"
#include "renderer/ShaderProgram.h"
#include "renderer/Texture2D.h"

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <filesystem>
#include <vector>

namespace novaiso::renderer {

class Renderer2D {
public:
    struct LightSource {
        glm::vec2 position{0.0f, 0.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        glm::vec2 direction{1.0f, 0.0f};
        float radius = 280.0f;
        float length = 520.0f;
        float source_radius = 26.0f;
        float scatter = 1.0f;
        float cone_angle = 42.0f;
        float cone_softness = 0.28f;
        float intensity = 1.0f;
        float type = 0.0f;
    };

    bool Initialize(const std::filesystem::path& shader_root);
    void Shutdown();

    void Begin(const camera::Camera2D& camera);
    void SetLightingState(bool lighting_enabled, bool rt_enabled, glm::vec4 ambient_color, float ambient_intensity, const std::vector<LightSource>& lights);
    void DrawSprite(
        const Texture2D& texture,
        glm::vec2 position,
        glm::vec2 size,
        glm::vec4 color,
        float depth = 0.0f,
        glm::vec4 uv = {0.0f, 0.0f, 1.0f, 1.0f},
        float rotation_radians = 0.0f,
        float reflection = 0.0f,
        glm::vec4 material = {0.0f, 0.0f, 0.0f, 1.0f},
        const Texture2D* normal_map = nullptr,
        const Texture2D* height_map = nullptr,
        const Texture2D* displacement_map = nullptr,
        glm::vec4 lighting = {1.0f, 0.0f, 0.0f, 0.0f},
        glm::vec4 emissive = {0.0f, 0.0f, 0.0f, 0.0f}
    );
    void DrawSpriteQuad(
        const Texture2D& texture,
        const std::array<glm::vec2, 4>& points,
        glm::vec4 color,
        float depth = 0.0f,
        glm::vec4 uv = {0.0f, 0.0f, 1.0f, 1.0f},
        float reflection = 0.0f,
        glm::vec4 material = {0.0f, 0.0f, 0.0f, 1.0f},
        const Texture2D* normal_map = nullptr,
        const Texture2D* height_map = nullptr,
        const Texture2D* displacement_map = nullptr,
        glm::vec4 lighting = {1.0f, 0.0f, 0.0f, 0.0f},
        glm::vec4 emissive = {0.0f, 0.0f, 0.0f, 0.0f}
    );
    void DrawLight(
        glm::vec2 position,
        glm::vec2 size,
        glm::vec4 color,
        float source_radius = 0.0f,
        float scatter = 1.0f,
        float depth = 0.0f,
        bool rt_enabled = false,
        float light_type = 0.0f,
        glm::vec2 direction = {1.0f, 0.0f},
        float cone_angle = 42.0f,
        float cone_softness = 0.28f
    );
    void DrawQuad(const std::array<glm::vec2, 4>& points, glm::vec4 color, float depth = 0.0f);
    void DrawQuadGradient(const std::array<glm::vec2, 4>& points, const std::array<glm::vec4, 4>& colors, float depth = 0.0f);
    void DrawReflectiveSpriteQuad(
        const Texture2D& texture,
        const std::array<glm::vec2, 4>& points,
        glm::vec4 color,
        float reflection,
        float depth = 0.0f,
        glm::vec4 uv = {0.0f, 0.0f, 1.0f, 1.0f},
        glm::vec4 material = {0.0f, 0.0f, 0.0f, 1.0f},
        const Texture2D* normal_map = nullptr,
        const Texture2D* height_map = nullptr,
        const Texture2D* displacement_map = nullptr,
        glm::vec4 lighting = {1.0f, 0.0f, 0.0f, 0.0f},
        glm::vec4 emissive = {0.0f, 0.0f, 0.0f, 0.0f}
    );
    void DrawRectOutline(glm::vec2 position, glm::vec2 size, glm::vec4 color, float thickness = 2.0f, float depth = 0.0f);
    void Flush();

    [[nodiscard]] Texture2D& WhiteTexture();
    [[nodiscard]] Texture2D& RadialLightTexture();

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec2 uv;
        glm::vec4 color;
        float reflection = 0.0f;
        glm::vec4 material{0.0f, 0.0f, 0.0f, 1.0f};
        glm::vec4 lighting{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4 emissive{0.0f, 0.0f, 0.0f, 0.0f};
    };

    void FlushInternal();
    void PushQuad(
        const Texture2D& texture,
        const Texture2D* normal_map,
        const Texture2D* height_map,
        const Texture2D* displacement_map,
        const std::array<Vertex, 4>& quad);

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    ShaderProgram sprite_shader_;
    ShaderProgram light_shader_;
    Texture2D white_texture_;
    Texture2D radial_light_texture_;
    Texture2D flat_normal_texture_;
    Texture2D neutral_height_texture_;
    Texture2D neutral_displacement_texture_;
    const Texture2D* active_texture_ = nullptr;
    const Texture2D* active_normal_texture_ = nullptr;
    const Texture2D* active_height_texture_ = nullptr;
    const Texture2D* active_displacement_texture_ = nullptr;
    glm::mat4 view_projection_{1.0f};
    bool lighting_enabled_ = false;
    bool rt_enabled_ = false;
    glm::vec4 ambient_color_{0.08f, 0.09f, 0.14f, 1.0f};
    float ambient_intensity_ = 0.35f;
    std::vector<LightSource> active_lights_;
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
};

}  // namespace novaiso::renderer
